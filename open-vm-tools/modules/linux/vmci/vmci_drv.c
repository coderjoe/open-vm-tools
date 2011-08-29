/*********************************************************
 * Copyright (C) 2005 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

/*
 * vmci.c --
 *
 *      Linux guest driver for the VMCI device.
 */

#include "driver-config.h"

#include <linux/moduleparam.h>
#include <linux/poll.h>

#include "compat_kernel.h"
#include "compat_module.h"
#include "compat_pci.h"
#include "compat_init.h"
#include "compat_ioport.h"
#include "compat_interrupt.h"
#include "compat_page.h"
#include "compat_mutex.h"
#include "vm_basic_types.h"
#include "vm_device_version.h"
#include "kernelStubs.h"
#include "vmci_iocontrols.h"
#include "vmci_defs.h"
#include "vmciInt.h"
#include "vmci_infrastructure.h"
#include "vmciDatagram.h"
#include "vmciProcess.h"
#include "vmciUtil.h"
#include "vmciEvent.h"
#include "vmciNotifications.h"
#include "vmciQueuePairInt.h"
#include "vmci_version.h"
#include "vmciCommonInt.h"

#define LGPFX "VMCI: "
#define VMCI_DEVICE_MINOR_NUM 0

/* MSI-X has performance problems in < 2.6.19 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 19)
#  define VMCI_DISABLE_MSIX   0
#else
#  define VMCI_DISABLE_MSIX   1
#endif

typedef struct vmci_device {
   compat_mutex_t    lock;

   unsigned int      ioaddr;
   unsigned int      ioaddr_size;
   unsigned int      irq;
   unsigned int      intr_type;
   Bool              exclusive_vectors;
   struct msix_entry msix_entries[VMCI_MAX_INTRS];

   Bool              enabled;
   spinlock_t        dev_spinlock;
} vmci_device;

static int vmci_probe_device(struct pci_dev *pdev,
                             const struct pci_device_id *id);
static void vmci_remove_device(struct pci_dev* pdev);
static int vmci_open(struct inode *inode, struct file *file);
static int vmci_close(struct inode *inode, struct file *file);
static long vmci_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
#if !defined(HAVE_UNLOCKED_IOCTL)
static int vmci_legacy_ioctl(struct inode *dummy, struct file *file,
                             unsigned int cmd, unsigned long arg);
#endif
static unsigned int vmci_poll(struct file *file, poll_table *wait);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static compat_irqreturn_t vmci_interrupt(int irq, void *dev_id,
                                           struct pt_regs * regs);
static compat_irqreturn_t vmci_interrupt_bm(int irq, void *dev_id,
                                            struct pt_regs * regs);
#else
static compat_irqreturn_t vmci_interrupt(int irq, void *dev_id);
static compat_irqreturn_t vmci_interrupt_bm(int irq, void *dev_id);
#endif
static void dispatch_datagrams(unsigned long data);
static void process_bitmap(unsigned long data);

static const struct pci_device_id vmci_ids[] = {
   { PCI_DEVICE(PCI_VENDOR_ID_VMWARE, PCI_DEVICE_ID_VMWARE_VMCI), },
   { 0 },
};

static struct file_operations vmci_ops = {
   .owner   = THIS_MODULE,
   .open    = vmci_open,
   .release = vmci_close,
#ifdef HAVE_UNLOCKED_IOCTL
   .unlocked_ioctl = vmci_ioctl,
#else
   .ioctl   = vmci_legacy_ioctl,
#endif
#ifdef HAVE_COMPAT_IOCTL
   .compat_ioctl = vmci_ioctl,
#endif
   .poll    = vmci_poll,
};

static struct pci_driver vmci_driver = {
   .name     = "vmci",
   .id_table = vmci_ids,
   .probe = vmci_probe_device,
   .remove = __devexit_p(vmci_remove_device),
};

static vmci_device vmci_dev;

/* We dynamically request the device major number at init time. */
static int device_major_nr = 0;

static int vmci_disable_msi;
static int vmci_disable_msix = VMCI_DISABLE_MSIX;

DECLARE_TASKLET(vmci_dg_tasklet, dispatch_datagrams,
                (unsigned long)&vmci_dev);

DECLARE_TASKLET(vmci_bm_tasklet, process_bitmap,
                (unsigned long)&vmci_dev);

/*
 * Allocate a buffer for incoming datagrams globally to avoid repeated
 * allocation in the interrupt handler's atomic context.
 */

static uint8 *data_buffer = NULL;
static uint32 data_buffer_size = VMCI_MAX_DG_SIZE;

/*
 * If the VMCI hardware supports the notification bitmap, we allocate
 * and register a page with the device.
 */

static uint8 *notification_bitmap = NULL;


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_init --
 *
 *      Initialization, called by Linux when the module is loaded.
 *
 * Results:
 *      Returns 0 for success, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int __init
vmci_init(void)
{
   int err;

   /* Initialize device data. */
   compat_mutex_init(&vmci_dev.lock);
   vmci_dev.intr_type = VMCI_INTR_TYPE_INTX;
   vmci_dev.exclusive_vectors = FALSE;
   spin_lock_init(&vmci_dev.dev_spinlock);
   vmci_dev.enabled = FALSE;

   data_buffer = vmalloc(data_buffer_size);
   if (!data_buffer) {
      return -ENOMEM;
   }

   /* Register device node ops. */
   err = register_chrdev(0, "vmci", &vmci_ops);
   if (err < 0) {
      printk(KERN_ERR "Unable to register vmci device\n");
      goto err_free_mem;
   }

   device_major_nr = err;

   printk("VMCI: Major device number is: %d\n", device_major_nr);

   /* This should be last to make sure we are done initializing. */
   err = pci_register_driver(&vmci_driver);
   if (err < 0) {
      goto err_unregister_chrdev;
   }

   return 0;

err_unregister_chrdev:
   unregister_chrdev(device_major_nr, "vmci");
err_free_mem:
   vfree(data_buffer);
   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_exit --
 *
 *      Cleanup, called by Linux when the module is unloaded.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void __exit
vmci_exit(void)
{
   pci_unregister_driver(&vmci_driver);

   unregister_chrdev(device_major_nr, "vmci");

   vfree(data_buffer);
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_enable_msix --
 *
 *      Enable MSI-X.  Try exclusive vectors first, then shared vectors.
 *
 * Results:
 *      0 on success, other error codes on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int
vmci_enable_msix(struct pci_dev *pdev) // IN
{
   int i;
   int result;

   for (i = 0; i < VMCI_MAX_INTRS; ++i) {
      vmci_dev.msix_entries[i].entry = i;
      vmci_dev.msix_entries[i].vector = i;
   }

   result = pci_enable_msix(pdev, vmci_dev.msix_entries, VMCI_MAX_INTRS);
   if (!result) {
      vmci_dev.exclusive_vectors = TRUE;
   } else if (result > 0) {
      result = pci_enable_msix(pdev, vmci_dev.msix_entries, 1);
   }
   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_probe_device --
 *
 *      Most of the initialization at module load time is done here.
 *
 * Results:
 *      Returns 0 for success, an error otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int __devinit
vmci_probe_device(struct pci_dev *pdev,           // IN: vmci PCI device
                  const struct pci_device_id *id) // IN: matching device ID
{
   unsigned int ioaddr;
   unsigned int ioaddr_size;
   unsigned int capabilities;
   int result;

   printk(KERN_INFO "Probing for vmci/PCI.\n");

   result = compat_pci_enable_device(pdev);
   if (result) {
      printk(KERN_ERR "Cannot VMCI device %s: error %d\n",
             compat_pci_name(pdev), result);
      return result;
   }
   compat_pci_set_master(pdev); /* To enable QueuePair functionality. */
   ioaddr = compat_pci_resource_start(pdev, 0);
   ioaddr_size = compat_pci_resource_len(pdev, 0);

   /*
    * Request I/O region with adjusted base address and size. The adjusted
    * values are needed and used if we release the region in case of failure.
    */

   if (!compat_request_region(ioaddr, ioaddr_size, "vmci")) {
      printk(KERN_INFO "vmci: Another driver already loaded "
                       "for device in slot %s.\n", compat_pci_name(pdev));
      goto pci_disable;
   }

   printk(KERN_INFO "Found vmci/PCI at %#x, irq %u.\n", ioaddr, pdev->irq);

   /*
    * Verify that the VMCI Device supports the capabilities that
    * we need. If the device is missing capabilities that we would
    * like to use, check for fallback capabilities and use those
    * instead (so we can run a new VM on old hosts). Fail the load if
    * a required capability is missing and there is no fallback.
    *
    * Right now, we need datagrams. There are no fallbacks.
    */
   capabilities = inl(ioaddr + VMCI_CAPS_ADDR);

   if ((capabilities & VMCI_CAPS_DATAGRAM) == 0) {
      printk(KERN_ERR "VMCI device does not support datagrams.\n");
      goto release;
   }

   /*
    * If the hardware supports notifications, we will use that as
    * well.
    */
   if (capabilities & VMCI_CAPS_NOTIFICATIONS) {
      capabilities = VMCI_CAPS_DATAGRAM;
      notification_bitmap = vmalloc(PAGE_SIZE);
      if (notification_bitmap == NULL) {
         printk(KERN_ERR "VMCI device unable to allocate notification bitmap.\n");
      } else {
         memset(notification_bitmap, 0, PAGE_SIZE);
         capabilities |= VMCI_CAPS_NOTIFICATIONS;
      }
   } else {
      capabilities = VMCI_CAPS_DATAGRAM;
   }
   printk(KERN_INFO "VMCI: using capabilities 0x%x.\n", capabilities);

   /* Let the host know which capabilities we intend to use. */
   outl(capabilities, ioaddr + VMCI_CAPS_ADDR);

   /* Device struct initialization. */
   compat_mutex_lock(&vmci_dev.lock);
   if (vmci_dev.enabled) {
      printk(KERN_ERR "VMCI device already enabled.\n");
      goto unlock;
   }

   vmci_dev.ioaddr = ioaddr;
   vmci_dev.ioaddr_size = ioaddr_size;

   /*
    * Register notification bitmap with device if that capability is
    * used
    */
   if (capabilities & VMCI_CAPS_NOTIFICATIONS) {
      unsigned long bitmapPPN;
      bitmapPPN = page_to_pfn(vmalloc_to_page(notification_bitmap));
      if (!VMCI_RegisterNotificationBitmap(bitmapPPN)) {
         printk(KERN_ERR "VMCI device unable to register notification bitmap "
                "with PPN 0x%x.\n", (uint32)bitmapPPN);
         goto unlock;
      }
   }

   /* Check host capabilities. */
   if (!VMCI_CheckHostCapabilities()) {
      goto remove_bitmap;
   }

   /* Enable device. */
   vmci_dev.enabled = TRUE;
   pci_set_drvdata(pdev, &vmci_dev);

   /*
    * We do global initialization here because we need datagrams for
    * event init. If we ever support more than one VMCI device we will
    * have to create seperate LateInit/EarlyExit functions that can be
    * used to do initialization/cleanup that depends on the device
    * being accessible.  We need to initialize VMCI components before
    * requesting an irq - the VMCI interrupt handler uses these
    * components, and it may be invoked once request_irq() has
    * registered the handler (as the irq line may be shared).
    */
   VMCIProcess_Init();
   VMCIDatagram_Init();
   VMCIEvent_Init();
   VMCIUtil_Init();
   VMCINotifications_Init();
   VMCIQueuePair_Init();

   /*
    * Enable interrupts.  Try MSI-X first, then MSI, and then fallback on
    * legacy interrupts.
    */
   if (!vmci_disable_msix && !vmci_enable_msix(pdev)) {
      vmci_dev.intr_type = VMCI_INTR_TYPE_MSIX;
      vmci_dev.irq = vmci_dev.msix_entries[0].vector;
   } else if (!vmci_disable_msi && !pci_enable_msi(pdev)) {
      vmci_dev.intr_type = VMCI_INTR_TYPE_MSI;
      vmci_dev.irq = pdev->irq;
   } else {
      vmci_dev.intr_type = VMCI_INTR_TYPE_INTX;
      vmci_dev.irq = pdev->irq;
   }

   /* Request IRQ for legacy or MSI interrupts, or for first MSI-X vector. */
   result = request_irq(vmci_dev.irq, vmci_interrupt, COMPAT_IRQF_SHARED,
                        "vmci", &vmci_dev);
   if (result) {
      printk(KERN_ERR "vmci: irq %u in use: %d\n", vmci_dev.irq, result);
      goto components_exit;
   }

   /*
    * For MSI-X with exclusive vectors we need to request an interrupt for each
    * vector so that we get a separate interrupt handler routine.  This allows
    * us to distinguish between the vectors.
    */

   if (vmci_dev.exclusive_vectors) {
      ASSERT(vmci_dev.intr_type == VMCI_INTR_TYPE_MSIX);
      result = request_irq(vmci_dev.msix_entries[1].vector, vmci_interrupt_bm,
                           0, "vmci", &vmci_dev);
      if (result) {
         printk(KERN_ERR "vmci: irq %u in use: %d\n",
                vmci_dev.msix_entries[1].vector, result);
         free_irq(vmci_dev.irq, &vmci_dev);
         goto components_exit;
      }
   }

   printk(KERN_INFO "Registered vmci device.\n");

   compat_mutex_unlock(&vmci_dev.lock);

   /* Enable specific interrupt bits. */
   if (capabilities & VMCI_CAPS_NOTIFICATIONS) {
      outl(VMCI_IMR_DATAGRAM | VMCI_IMR_NOTIFICATION,
           vmci_dev.ioaddr + VMCI_IMR_ADDR);
   } else {
      outl(VMCI_IMR_DATAGRAM, vmci_dev.ioaddr + VMCI_IMR_ADDR);
   }

   /* Enable interrupts. */
   outl(VMCI_CONTROL_INT_ENABLE, vmci_dev.ioaddr + VMCI_CONTROL_ADDR);

   return 0;

 components_exit:
   VMCIQueuePair_Exit();
   VMCINotifications_Exit();
   VMCIUtil_Exit();
   VMCIEvent_Exit();
   VMCIProcess_Exit();
   if (vmci_dev.intr_type == VMCI_INTR_TYPE_MSIX) {
      pci_disable_msix(pdev);
   } else if (vmci_dev.intr_type == VMCI_INTR_TYPE_MSI) {
      pci_disable_msi(pdev);
   }
 remove_bitmap:
   if (notification_bitmap) {
      outl(VMCI_CONTROL_RESET, vmci_dev.ioaddr + VMCI_CONTROL_ADDR);
   }
 unlock:
   compat_mutex_unlock(&vmci_dev.lock);
 release:
   if (notification_bitmap) {
      vfree(notification_bitmap);
   }
   release_region(ioaddr, ioaddr_size);
 pci_disable:
   compat_pci_disable_device(pdev);
   return -EBUSY;
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_remove_device --
 *
 *      Cleanup, called for each device on unload.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void __devexit
vmci_remove_device(struct pci_dev* pdev)
{
   struct vmci_device *dev = pci_get_drvdata(pdev);

   printk(KERN_INFO "Removing vmci device\n");

   VMCIQueuePair_Exit();

   // XXX Todo add exit/cleanup functions for util, sm, dg, and resource apis.
   VMCIUtil_Exit();
   VMCIEvent_Exit();
   //VMCIDatagram_Exit();
   VMCIProcess_Exit();

   compat_mutex_lock(&dev->lock);
   printk(KERN_INFO "Resetting vmci device\n");
   outl(VMCI_CONTROL_RESET, vmci_dev.ioaddr + VMCI_CONTROL_ADDR);

   /*
    * Free IRQ and then disable MSI/MSI-X as appropriate.  For MSI-X, we might
    * have multiple vectors, each with their own IRQ, which we must free too.
    */

   free_irq(dev->irq, dev);
   if (dev->intr_type == VMCI_INTR_TYPE_MSIX) {
      if (dev->exclusive_vectors) {
         free_irq(dev->msix_entries[1].vector, dev);
      }
      pci_disable_msix(pdev);
   } else if (dev->intr_type == VMCI_INTR_TYPE_MSI) {
      pci_disable_msi(pdev);
   }
   dev->exclusive_vectors = FALSE;
   dev->intr_type = VMCI_INTR_TYPE_INTX;

   release_region(dev->ioaddr, dev->ioaddr_size);
   dev->enabled = FALSE;
   VMCINotifications_Exit();
   if (notification_bitmap) {
      vfree(notification_bitmap);
   }

   printk(KERN_INFO "Unregistered vmci device.\n");
   compat_mutex_unlock(&dev->lock);

   compat_pci_disable_device(pdev);
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_open --
 *
 *      Open device.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int
vmci_open(struct inode *inode,  // IN
          struct file *file)    // IN
{
   VMCIGuestDeviceHandle *devHndl;
   int errcode;

   printk(KERN_INFO "Opening vmci device\n");

   if (MINOR(inode->i_rdev) != VMCI_DEVICE_MINOR_NUM) {
      return -ENODEV;
   }

   compat_mutex_lock(&vmci_dev.lock);
   if (!vmci_dev.enabled) {
      printk(KERN_INFO "Received open on uninitialized vmci device.\n");
      errcode = -ENODEV;
      goto unlock;
   }

   /* Do open ... */
   devHndl = VMCI_AllocKernelMem(sizeof *devHndl, VMCI_MEMORY_NORMAL);
   if (!devHndl) {
      printk(KERN_INFO "Failed to create device obj when opening device.\n");
      errcode = -ENOMEM;
      goto unlock;
   }
   devHndl->obj = NULL;
   devHndl->objType = VMCIOBJ_NOT_SET;
   file->private_data = devHndl;

   compat_mutex_unlock(&vmci_dev.lock);

   return 0;

 unlock:
   compat_mutex_unlock(&vmci_dev.lock);
   return errcode;
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_close --
 *
 *      Close device.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int
vmci_close(struct inode *inode,  // IN
           struct file *file)    // IN
{
   VMCIGuestDeviceHandle *devHndl =
      (VMCIGuestDeviceHandle *) file->private_data;

   if (devHndl) {
      if (devHndl->objType == VMCIOBJ_PROCESS) {
         VMCIProcess_Destroy((VMCIProcess *) devHndl->obj);
      } else if (devHndl->objType == VMCIOBJ_DATAGRAM_PROCESS) {
         VMCIDatagramProcess_Destroy((VMCIDatagramProcess *) devHndl->obj);
      }
      VMCI_FreeKernelMem(devHndl, sizeof *devHndl);
      file->private_data = NULL;
   }
   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_ioctl --
 *
 *      IOCTL interface to device.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static long
vmci_ioctl(struct file *file,    // IN
           unsigned int cmd,     // IN
           unsigned long arg)    // IN
{
#ifndef VMX86_DEVEL
   return -ENOTTY;
#else
   int retval;
   VMCIGuestDeviceHandle *devHndl = file->private_data;

   if (devHndl == NULL) {
      return -EINVAL;
   }

   compat_mutex_lock(&vmci_dev.lock);

   /*
    * When adding new ioctls make sure that their data structures are same
    * for i386 and x86_64 architectures, as this handler is used for both ia32
    * and x86_64 ioctls.
    */
   switch (cmd) {
   case IOCTL_VMCI_CREATE_PROCESS: {
      if (devHndl->objType != VMCIOBJ_NOT_SET) {
         printk("VMCI: Received IOCTLCMD_VMCI_CREATE_PROCESS on "
                "initialized handle.\n");
         retval = -EINVAL;
         break;
      }
      ASSERT(!devHndl->obj);
      retval = VMCIProcess_Create((VMCIProcess **) &devHndl->obj);
      if (retval != 0) {
         printk("VMCI: Failed to create process.\n");
         break;
      }
      devHndl->objType = VMCIOBJ_PROCESS;
      break;
   }

   case IOCTL_VMCI_CREATE_DATAGRAM_PROCESS: {
      VMCIDatagramCreateProcessInfo createInfo;
      VMCIDatagramProcess *dgmProc;

      if (devHndl->objType != VMCIOBJ_NOT_SET) {
         printk("VMCI: Received IOCTLCMD_VMCI_CREATE_DATAGRAM_PROCESS on "
                "initialized handle.\n");
         retval = -EINVAL;
         break;
      }
      ASSERT(!devHndl->obj);

      retval = copy_from_user(&createInfo, (void *)arg, sizeof createInfo);
      if (retval != 0) {
	 printk("VMCI: Error getting datagram create info, %d.\n", retval);
	 retval = -EFAULT;
	 break;
      }

      if (VMCIDatagramProcess_Create(&dgmProc, &createInfo,
                                     0 /* Unused */) < VMCI_SUCCESS) {
	 retval = -EINVAL;
	 break;
      }

      retval = copy_to_user((void *)arg, &createInfo, sizeof createInfo);
      if (retval != 0) {
         VMCIDatagramProcess_Destroy(dgmProc);
         printk("VMCI: Failed to create datagram process.\n");
	 retval = -EFAULT;
         break;
      }
      devHndl->obj = dgmProc;
      devHndl->objType = VMCIOBJ_DATAGRAM_PROCESS;
      break;
   }

   case IOCTL_VMCI_DATAGRAM_SEND: {
      VMCIDatagramSendRecvInfo sendInfo;
      VMCIDatagram *dg;

      if (devHndl->objType != VMCIOBJ_DATAGRAM_PROCESS) {
         printk("VMCI: Ioctl %d only valid for process datagram handle.\n",
		cmd);
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&sendInfo, (void *) arg, sizeof sendInfo);
      if (retval) {
         printk("VMCI: copy_from_user failed.\n");
         retval = -EFAULT;
         break;
      }

      if (sendInfo.len > VMCI_MAX_DG_SIZE) {
         printk("VMCI: datagram size too big.\n");
	 retval = -EINVAL;
	 break;
      }

      dg = VMCI_AllocKernelMem(sendInfo.len, VMCI_MEMORY_NORMAL);
      if (dg == NULL) {
         printk("VMCI: Cannot allocate memory to dispatch datagram.\n");
         retval = -ENOMEM;
         break;
      }

      retval = copy_from_user(dg, (char *)(VA)sendInfo.addr, sendInfo.len);
      if (retval != 0) {
         printk("VMCI: Error getting datagram: %d\n", retval);
         VMCI_FreeKernelMem(dg, sendInfo.len);
         retval = -EFAULT;
         break;
      }

      DEBUG_ONLY(printk("VMCI: Datagram dst handle 0x%x:0x%x, src handle "
			"0x%x:0x%x, payload size %"FMT64"u.\n",
			dg->dst.context, dg->dst.resource,
			dg->src.context, dg->src.resource, dg->payloadSize));

      sendInfo.result = VMCIDatagram_Send(dg);
      VMCI_FreeKernelMem(dg, sendInfo.len);

      retval = copy_to_user((void *)arg, &sendInfo, sizeof sendInfo);
      break;
   }

   case IOCTL_VMCI_DATAGRAM_RECEIVE: {
      VMCIDatagramSendRecvInfo recvInfo;
      VMCIDatagram *dg = NULL;

      if (devHndl->objType != VMCIOBJ_DATAGRAM_PROCESS) {
         printk("VMCI: Ioctl %d only valid for process datagram handle.\n",
		cmd);
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&recvInfo, (void *) arg, sizeof recvInfo);
      if (retval) {
         printk("VMCI: copy_from_user failed.\n");
         retval = -EFAULT;
         break;
      }

      ASSERT(devHndl->obj);
      recvInfo.result =
	 VMCIDatagramProcess_ReadCall((VMCIDatagramProcess *)devHndl->obj,
				      recvInfo.len, &dg);
      if (recvInfo.result < VMCI_SUCCESS) {
	 retval = -EINVAL;
	 break;
      }
      ASSERT(dg);
      retval = copy_to_user((void *) ((uintptr_t) recvInfo.addr), dg,
			    VMCI_DG_SIZE(dg));
      VMCI_FreeKernelMem(dg, VMCI_DG_SIZE(dg));
      if (retval != 0) {
	 break;
      }
      retval = copy_to_user((void *)arg, &recvInfo, sizeof recvInfo);
      break;
   }

   case IOCTL_VMCI_GET_CONTEXT_ID: {
      VMCIId cid = VMCI_GetContextID();

      retval = copy_to_user((void *)arg, &cid, sizeof cid);
      break;
   }
 
   default:
      printk(KERN_DEBUG "vmci_ioctl(): unknown ioctl 0x%x.\n", cmd);
      retval = -EINVAL;
      break;
   }

   compat_mutex_unlock(&vmci_dev.lock);

   return retval;
#endif
}


#if !defined(HAVE_UNLOCKED_IOCTL)
/*
 *-----------------------------------------------------------------------------
 *
 * vmci_legacy_ioctl --
 *
 *      IOCTL interface for kernels that do not have unlocked_ioctl.
 *
 * Results:
 *      Negative error code, or per-ioctl result.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int
vmci_legacy_ioctl(struct inode *inode,  // IN: unused
                  struct file *filp,    // IN:
                  u_int iocmd,          // IN:
                  unsigned long ioarg)  // IN:
{
   return vmci_ioctl(filp, iocmd, ioarg);
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_poll --
 *
 *      vmci poll function
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static unsigned int
vmci_poll(struct file *file, // IN
          poll_table *wait)  // IN
{
   VMCILockFlags flags;
   unsigned int mask = 0;
   VMCIGuestDeviceHandle *devHndl =
      (VMCIGuestDeviceHandle *) file->private_data;

   /*
    * Check for call to this VMCI process.
    */

   if (!devHndl) {
      return mask;
   }
   if (devHndl->objType == VMCIOBJ_DATAGRAM_PROCESS) {
      VMCIDatagramProcess *dgmProc = (VMCIDatagramProcess *) devHndl->obj;
      ASSERT(dgmProc);

      if (wait != NULL) {
         poll_wait(file, &dgmProc->host.waitQueue, wait);
      }

      VMCI_GrabLock_BH(&dgmProc->datagramQueueLock, &flags);
      if (dgmProc->pendingDatagrams > 0) {
         mask = POLLIN;
      }
      VMCI_ReleaseLock_BH(&dgmProc->datagramQueueLock, flags);
   }

   return mask;
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_interrupt --
 *
 *      Interrupt handler for legacy or MSI interrupt, or for first MSI-X
 *      interrupt (vector VMCI_INTR_DATAGRAM).
 *
 * Results:
 *      COMPAT_IRQ_HANDLED if the interrupt is handled, COMPAT_IRQ_NONE if
 *      not an interrupt.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static compat_irqreturn_t
vmci_interrupt(int irq,               // IN
               void *clientdata,      // IN
               struct pt_regs *regs)  // IN
#else
static compat_irqreturn_t
vmci_interrupt(int irq,               // IN
               void *clientdata)      // IN
#endif
{
   vmci_device *dev = clientdata;

   if (dev == NULL) {
      printk(KERN_DEBUG "vmci_interrupt(): irq %d for unknown device.\n", irq);
      return COMPAT_IRQ_NONE;
   }

   /*
    * If we are using MSI-X with exclusive vectors then we simply schedule
    * the datagram tasklet, since we know the interrupt was meant for us.
    * Otherwise we must read the ICR to determine what to do.
    */

   if (dev->intr_type == VMCI_INTR_TYPE_MSIX && dev->exclusive_vectors) {
      tasklet_schedule(&vmci_dg_tasklet);
   } else {
      unsigned int icr;

      ASSERT(dev->intr_type == VMCI_INTR_TYPE_INTX ||
             dev->intr_type == VMCI_INTR_TYPE_MSI);

      /* Acknowledge interrupt and determine what needs doing. */
      icr = inl(dev->ioaddr + VMCI_ICR_ADDR);
      if (icr == 0 || icr == 0xffffffff) {
         return COMPAT_IRQ_NONE;
      }

      if (icr & VMCI_ICR_DATAGRAM) {
         tasklet_schedule(&vmci_dg_tasklet);
         icr &= ~VMCI_ICR_DATAGRAM;
      }
      if (icr & VMCI_ICR_NOTIFICATION) {
         tasklet_schedule(&vmci_bm_tasklet);
         icr &= ~VMCI_ICR_NOTIFICATION;
      }
      if (icr != 0) {
         printk(KERN_INFO LGPFX"Ignoring unknown interrupt cause (%d).\n", icr);
      }
   }

   return COMPAT_IRQ_HANDLED;
}


/*
 *-----------------------------------------------------------------------------
 *
 * vmci_interrupt_bm --
 *
 *      Interrupt handler for MSI-X interrupt vector VMCI_INTR_NOTIFICATION,
 *      which is for the notification bitmap.  Will only get called if we are
 *      using MSI-X with exclusive vectors.
 *
 * Results:
 *      COMPAT_IRQ_HANDLED if the interrupt is handled, COMPAT_IRQ_NONE if
 *      not an interrupt.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static compat_irqreturn_t
vmci_interrupt_bm(int irq,               // IN
                  void *clientdata,      // IN
                  struct pt_regs *regs)  // IN
#else
static compat_irqreturn_t
vmci_interrupt_bm(int irq,               // IN
                  void *clientdata)      // IN
#endif
{
   vmci_device *dev = clientdata;

   if (dev == NULL) {
      printk(KERN_DEBUG "vmci_interrupt_bm(): irq %d for unknown device.\n", irq);
      return COMPAT_IRQ_NONE;
   }

   /* For MSI-X we can just assume it was meant for us. */
   ASSERT(dev->intr_type == VMCI_INTR_TYPE_MSIX && dev->exclusive_vectors);
   tasklet_schedule(&vmci_bm_tasklet);

   return COMPAT_IRQ_HANDLED;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_DeviceEnabled --
 *
 *      Checks whether the VMCI device is enabled.
 *
 * Results:
 *      TRUE if device is enabled, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VMCI_DeviceEnabled(void)
{
   Bool retval;

   compat_mutex_lock(&vmci_dev.lock);
   retval = vmci_dev.enabled;
   compat_mutex_unlock(&vmci_dev.lock);

   return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_SendDatagram --
 *
 *      VM to hypervisor call mechanism. We use the standard VMware naming
 *      convention since shared code is calling this function as well.
 *
 * Results:
 *      The result of the hypercall.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCI_SendDatagram(VMCIDatagram *dg)
{
   unsigned long flags;
   int result;

   /* Check args. */
   if (dg == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   /*
    * Need to acquire spinlock on the device because
    * the datagram data may be spread over multiple pages and the monitor may
    * interleave device user rpc calls from multiple VCPUs. Acquiring the
    * spinlock precludes that possibility. Disabling interrupts to avoid
    * incoming datagrams during a "rep out" and possibly landing up in this
    * function.
    */
   spin_lock_irqsave(&vmci_dev.dev_spinlock, flags);

   /*
    * Send the datagram and retrieve the return value from the result register.
    */
   __asm__ __volatile__(
      "cld\n\t"
      "rep outsb\n\t"
      : /* No output. */
      : "d"(vmci_dev.ioaddr + VMCI_DATA_OUT_ADDR),
        "c"(VMCI_DG_SIZE(dg)), "S"(dg)
      );

   /*
    * XXX Should read result high port as well when updating handlers to
    * return 64bit.
    */
   result = inl(vmci_dev.ioaddr + VMCI_RESULT_LOW_ADDR);
   spin_unlock_irqrestore(&vmci_dev.dev_spinlock, flags);

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * dispatch_datagrams --
 *
 *      Reads and dispatches incoming datagrams.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Reads data from the device.
 *
 *-----------------------------------------------------------------------------
 */

void
dispatch_datagrams(unsigned long data)
{
   vmci_device *dev = (vmci_device *)data;

   if (dev == NULL) {
      printk(KERN_DEBUG "vmci: dispatch_datagrams(): no vmci device"
	     "present.\n");
      return;
   }

   if (data_buffer == NULL) {
      printk(KERN_DEBUG "vmci: dispatch_datagrams(): no buffer present.\n");
      return;
   }


   VMCI_ReadDatagramsFromPort((VMCIIoHandle) 0, dev->ioaddr + VMCI_DATA_IN_ADDR,
			      data_buffer, data_buffer_size);
}


/*
 *-----------------------------------------------------------------------------
 *
 * process_bitmap --
 *
 *      Scans the notification bitmap for raised flags, clears them
 *      and handles the notifications.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
process_bitmap(unsigned long data)
{
   vmci_device *dev = (vmci_device *)data;

   if (dev == NULL) {
      printk(KERN_DEBUG "vmci: process_bitmaps(): no vmci device"
	     "present.\n");
      return;
   }

   if (notification_bitmap == NULL) {
      printk(KERN_DEBUG "vmci: process_bitmaps(): no bitmap present.\n");
      return;
   }


   VMCI_ScanNotificationBitmap(notification_bitmap);
}


module_init(vmci_init);
module_exit(vmci_exit);
MODULE_DEVICE_TABLE(pci, vmci_ids);

module_param_named(disable_msi, vmci_disable_msi, bool, 0);
MODULE_PARM_DESC(disable_msi, "Disable MSI use in driver - (default=0)");

module_param_named(disable_msix, vmci_disable_msix, bool, 0);
MODULE_PARM_DESC(disable_msix, "Disable MSI-X use in driver - (default="
		 __stringify(VMCI_DISABLE_MSIX) ")");

/* Module information. */
MODULE_AUTHOR("VMware, Inc.");
MODULE_DESCRIPTION("VMware Virtual Machine Communication Interface");
MODULE_VERSION(VMCI_DRIVER_VERSION_STRING);
MODULE_LICENSE("GPL v2");
/*
 * Starting with SLE10sp2, Novell requires that IHVs sign a support agreement
 * with them and mark their kernel modules as externally supported via a
 * change to the module header. If this isn't done, the module will not load
 * by default (i.e., neither mkinitrd nor modprobe will accept it).
 */
MODULE_INFO(supported, "external");
