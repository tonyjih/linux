/****************************************************************************
*
*    Copyright (C) 2005 - 2012 by Vivante Corp.
*
*    This program is free software; you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation; either version 2 of the license, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not write to the Free Software
*    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*****************************************************************************/



#include <linux/clk.h>
#include <linux/device.h>
#include <linux/slab.h>

#include "gc_hal_kernel_linux.h"

#if USE_PLATFORM_DRIVER
#   include <linux/of.h>
#   include <linux/platform_device.h>
#endif

#ifdef CONFIG_PXA_DVFM
#   include <mach/dvfm.h>
#   include <mach/pxa3xx_dvfm.h>
#endif


/* Zone used for header/footer. */
#define _GC_OBJ_ZONE    gcvZONE_DRIVER

MODULE_DESCRIPTION("Vivante Graphics Driver");
MODULE_LICENSE("GPL");

#ifdef CONFIG_DOVE_GPU
#   define DEVICE_NAME "dove_gpu"
#else
#   define DEVICE_NAME "galcore"
#endif

static struct class* gpuClass;

static gckGALDEVICE galDevice;

static int major = 199;
module_param(major, int, 0644);

#ifdef CONFIG_MACH_JZ4770

#ifndef IRQ_GPU
#define IRQ_GPU 6
#endif
#ifndef GPU_BASE
#define GPU_BASE 0x13040000
#endif
#ifndef JZ_GPU_MEM_BASE
#define JZ_GPU_MEM_BASE 0       /* if GPU_MEM_BASE = 0, alloc gpu memory dynamicly on bootup */
#endif
#ifndef JZ_GPU_MEM_SIZE
#define JZ_GPU_MEM_SIZE 0x400000    /* set default reserved memory 4M Bytes. */
#endif

static int irqLine = IRQ_GPU;
module_param(irqLine, int, 0644);

static long registerMemBase = GPU_BASE;
module_param(registerMemBase, long, 0644);

static ulong registerMemSize = 256 << 10;
module_param(registerMemSize, ulong, 0644);

static int irqLine2D = -1;
module_param(irqLine2D, int, 0644);

static long registerMemBase2D = 0x00000000;
module_param(registerMemBase2D, long, 0644);

static ulong registerMemSize2D = 256 << 10;
module_param(registerMemSize2D, ulong, 0644);

static long contiguousSize = JZ_GPU_MEM_SIZE;
module_param(contiguousSize, long, 0644);

static ulong contiguousBase = JZ_GPU_MEM_BASE;
module_param(contiguousBase, ulong, 0644);

#else /* CONFIG_MACH_JZ4770 */

static int irqLine = -1;
module_param(irqLine, int, 0644);

static long registerMemBase = 0x80000000;
module_param(registerMemBase, long, 0644);

static ulong registerMemSize = 256 << 10;
module_param(registerMemSize, ulong, 0644);

static int irqLine2D = -1;
module_param(irqLine2D, int, 0644);

static long registerMemBase2D = 0x00000000;
module_param(registerMemBase2D, long, 0644);

static ulong registerMemSize2D = 256 << 10;
module_param(registerMemSize2D, ulong, 0644);

static long contiguousSize = 4 << 20;
module_param(contiguousSize, long, 0644);

static ulong contiguousBase = 0;
module_param(contiguousBase, ulong, 0644);
#endif  /* CONFIG_MACH_JZ4770 */

static long bankSize = 32 << 20;
module_param(bankSize, long, 0644);

static int fastClear = -1;
module_param(fastClear, int, 0644);

static int compression = -1;
module_param(compression, int, 0644);

static int signal = 48;
module_param(signal, int, 0644);

static ulong baseAddress = 0;
module_param(baseAddress, ulong, 0644);

static ulong physSize = 0;
module_param(physSize, ulong, 0644);

static int showArgs = 1;
module_param(showArgs, int, 0644);

static int drv_open(
    struct inode* inode,
    struct file* filp
    );

static int drv_release(
    struct inode* inode,
    struct file* filp
    );

static long drv_ioctl(
    struct file* filp,
    unsigned int ioctlCode,
    unsigned long arg
    );

static int drv_mmap(
    struct file* filp,
    struct vm_area_struct* vma
    );

static struct file_operations driver_fops =
{
    .open       = drv_open,
    .release    = drv_release,
    .unlocked_ioctl = drv_ioctl,
    .mmap       = drv_mmap,
};

int drv_open(
    struct inode* inode,
    struct file* filp
    )
{
    gceSTATUS status;
    int attached = gcvFALSE;
    gcsHAL_PRIVATE_DATA_PTR data = NULL;
    int i;

    gcmkHEADER_ARG("inode=0x%08X filp=0x%08X", inode, filp);

    if (filp == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): filp is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    data = kmalloc(sizeof(gcsHAL_PRIVATE_DATA), GFP_KERNEL | __GFP_NOWARN);

    if (data == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): private_data is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_OUT_OF_MEMORY);
    }

    data->device             = galDevice;
    data->mappedMemory       = NULL;
    data->contiguousLogical  = NULL;
    data->pidOpen            = task_tgid_vnr(current);

    /* Attached the process. */
    for (i = 0; i < gcdCORE_COUNT; i++)
    {
        if (galDevice->kernels[i] != NULL)
        {
            gcmkONERROR(gckKERNEL_AttachProcess(galDevice->kernels[i], gcvTRUE));
        }
    }
    attached = gcvTRUE;

    if (!galDevice->contiguousMapped)
    {
        gcmkONERROR(gckOS_MapMemory(
            galDevice->os,
            galDevice->contiguousPhysical,
            galDevice->contiguousSize,
            &data->contiguousLogical
            ));

        for (i = 0; i < gcdCORE_COUNT; i++)
        {
            if (galDevice->kernels[i] != NULL)
            {
                gcmkVERIFY_OK(gckKERNEL_AddProcessDB(
                    galDevice->kernels[i],
                    data->pidOpen,
                    gcvDB_MAP_MEMORY,
                    data->contiguousLogical,
                    galDevice->contiguousPhysical,
                    galDevice->contiguousSize));
            }
        }
    }

    filp->private_data = data;

    /* Success. */
    gcmkFOOTER_NO();
    return 0;

OnError:
    if (data != NULL)
    {
        if (data->contiguousLogical != NULL)
        {
            gcmkVERIFY_OK(gckOS_UnmapMemory(
                galDevice->os,
                galDevice->contiguousPhysical,
                galDevice->contiguousSize,
                data->contiguousLogical
                ));
        }

        kfree(data);
    }

    if (attached)
    {
        for (i = 0; i < gcdCORE_COUNT; i++)
        {
            if (galDevice->kernels[i] != NULL)
            {
                gcmkVERIFY_OK(gckKERNEL_AttachProcess(galDevice->kernels[i], gcvFALSE));
            }
        }
    }

    gcmkFOOTER();
    return -ENOTTY;
}

int drv_release(
    struct inode* inode,
    struct file* filp
    )
{
    gceSTATUS status;
    gcsHAL_PRIVATE_DATA_PTR data;
    gckGALDEVICE device;
    int i;
    u32 processID;


    gcmkHEADER_ARG("inode=0x%08X filp=0x%08X", inode, filp);

    if (filp == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): filp is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    data = filp->private_data;

    if (data == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): private_data is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    device = data->device;

    if (device == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): device is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    if (!device->contiguousMapped)
    {
        if (data->contiguousLogical != NULL)
        {
	    processID = task_tgid_vnr(current);
            gcmkONERROR(gckOS_UnmapMemoryEx(
                galDevice->os,
                galDevice->contiguousPhysical,
                galDevice->contiguousSize,
                data->contiguousLogical,
                data->pidOpen
                ));

            for (i = 0; i < gcdCORE_COUNT; i++)
            {
                if (galDevice->kernels[i] != NULL)
                {
                    gcmkVERIFY_OK(
                         gckKERNEL_RemoveProcessDB(galDevice->kernels[i],
                                                   processID, gcvDB_MAP_MEMORY,
                                                   data->contiguousLogical));
                }
            }

            data->contiguousLogical = NULL;
        }
    }

    /* Clean user signals if exit unnormally. */
    processID = task_tgid_vnr(current);
    gcmkVERIFY_OK(gckOS_CleanProcessSignal(galDevice->os, (gctHANDLE)processID));

    /* A process gets detached. */
    for (i = 0; i < gcdCORE_COUNT; i++)
    {
        if (galDevice->kernels[i] != NULL)
        {
            gcmkONERROR(gckKERNEL_AttachProcessEx(galDevice->kernels[i], gcvFALSE, data->pidOpen));
        }
    }

    kfree(data);
    filp->private_data = NULL;

    /* Success. */
    gcmkFOOTER_NO();
    return 0;

OnError:
    gcmkFOOTER();
    return -ENOTTY;
}

long drv_ioctl(
    struct file* filp,
    unsigned int ioctlCode,
    unsigned long arg
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;
    u32 copyLen;
    DRIVER_ARGS drvArgs;
    gckGALDEVICE device;
    gcsHAL_PRIVATE_DATA_PTR data;
    s32 i, count;

    gcmkHEADER_ARG(
        "filp=0x%08X ioctlCode=0x%08X arg=0x%08X",
        filp, ioctlCode, arg
        );

    if (filp == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): filp is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    data = filp->private_data;

    if (data == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): private_data is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    device = data->device;

    if (device == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): device is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    if ((ioctlCode != IOCTL_GCHAL_INTERFACE)
    &&  (ioctlCode != IOCTL_GCHAL_KERNEL_INTERFACE)
    )
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): unknown command %d\n",
            __FUNCTION__, __LINE__,
            ioctlCode
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    /* Get the drvArgs. */
    copyLen = copy_from_user(
        &drvArgs, (void *) arg, sizeof(DRIVER_ARGS)
        );

    if (copyLen != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): error copying of the input arguments.\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    /* Now bring in the gcsHAL_INTERFACE structure. */
    if ((drvArgs.InputBufferSize  != sizeof(gcsHAL_INTERFACE))
    ||  (drvArgs.OutputBufferSize != sizeof(gcsHAL_INTERFACE))
    )
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): error copying of the input arguments.\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    copyLen = copy_from_user(
        &iface, drvArgs.InputBuffer, sizeof(gcsHAL_INTERFACE)
        );

    if (copyLen != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): error copying of input HAL interface.\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    if (iface.command == gcvHAL_CHIP_INFO)
    {
        count = 0;
        for (i = 0; i < gcdCORE_COUNT; i++)
        {
            if (device->kernels[i] != NULL)
            {
                gcmkVERIFY_OK(gckHARDWARE_GetType(device->kernels[i]->hardware,
                                                  &iface.u.ChipInfo.types[count]));

                count++;
            }
        }

        iface.u.ChipInfo.count = count;
        status = gcvSTATUS_OK;
    }
    else
    {
        if (iface.hardwareType < 0 || iface.hardwareType > 7)
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): unknown hardwareType %d\n",
                __FUNCTION__, __LINE__,
                iface.hardwareType
                );

            gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
        }

        status = gckKERNEL_Dispatch(device->kernels[device->coreMapping[iface.hardwareType]],
                                    (ioctlCode == IOCTL_GCHAL_INTERFACE),
                                    &iface);
    }

    /* Redo system call after pending signal is handled. */
    if (status == gcvSTATUS_INTERRUPTED)
    {
        gcmkFOOTER();
        return -ERESTARTSYS;
    }

    if (gcmIS_SUCCESS(status) && (iface.command == gcvHAL_LOCK_VIDEO_MEMORY))
    {
        /* Special case for mapped memory. */
        if ((data->mappedMemory != NULL)
        &&  (iface.u.LockVideoMemory.node->VidMem.memory->object.type == gcvOBJ_VIDMEM)
        )
        {
            /* Compute offset into mapped memory. */
            u32 offset
                = (u8 *) iface.u.LockVideoMemory.memory
                - (u8 *) device->contiguousBase;

            /* Compute offset into user-mapped region. */
            iface.u.LockVideoMemory.memory =
                (u8 *) data->mappedMemory + offset;
        }
    }

    /* Copy data back to the user. */
    copyLen = copy_to_user(
        drvArgs.OutputBuffer, &iface, sizeof(gcsHAL_INTERFACE)
        );

    if (copyLen != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): error copying of output HAL interface.\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    /* Success. */
    gcmkFOOTER_NO();
    return 0;

OnError:
    gcmkFOOTER();
    return -ENOTTY;
}

static int drv_mmap(
    struct file* filp,
    struct vm_area_struct* vma
    )
{
    gceSTATUS status;
    gcsHAL_PRIVATE_DATA_PTR data;
    gckGALDEVICE device;

    gcmkHEADER_ARG("filp=0x%08X vma=0x%08X", filp, vma);

    if (filp == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): filp is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    data = filp->private_data;

    if (data == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): private_data is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

    device = data->device;

    if (device == NULL)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): device is NULL\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

#if !gcdPAGED_MEMORY_CACHEABLE
    vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    vma->vm_flags    |= VM_IO | VM_DONTCOPY | VM_DONTEXPAND;
#endif
    vma->vm_pgoff     = 0;

    if (device->contiguousMapped)
    {
        unsigned long size = vma->vm_end - vma->vm_start;

        int ret = io_remap_pfn_range(
            vma,
            vma->vm_start,
            (u32) device->contiguousPhysical >> PAGE_SHIFT,
            size,
            vma->vm_page_prot
            );

        if (ret != 0)
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): io_remap_pfn_range failed %d\n",
                __FUNCTION__, __LINE__,
                ret
                );

            data->mappedMemory = NULL;

            gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
        }

        data->mappedMemory = (void *) vma->vm_start;
    }

    /* Success. */
    gcmkFOOTER_NO();
    return 0;

OnError:
    gcmkFOOTER();
    return -ENOTTY;
}

#if USE_PLATFORM_DRIVER
static int drv_init(struct device *dev)
{
#else
static int __init drv_init(void)
{
    struct device *dev = NULL;
#endif
    int ret;
    int result = -EINVAL;
    gceSTATUS status;
    gckGALDEVICE device = NULL;
    struct class* device_class = NULL;
    struct clk *clk;

    gcmkHEADER();

    clk = clk_get(dev, "gpu");
    if (IS_ERR(clk)) {
        pr_err(DEVICE_NAME ": cannot get \"gpu\" clock: %ld\n", PTR_ERR(clk));
        gcmkONERROR(gcvSTATUS_NOT_FOUND);
    }
#if !USE_PLATFORM_DRIVER
    clk_set_rate(clk, 800000000UL);
#endif
    clk_prepare_enable(clk);

    if (showArgs)
    {
        printk("galcore options:\n");
        printk("  irqLine           = %d\n",      irqLine);
        printk("  registerMemBase   = 0x%08lX\n", registerMemBase);
        printk("  registerMemSize   = 0x%08lX\n", registerMemSize);

        if (irqLine2D != -1)
        {
            printk("  irqLine2D         = %d\n",      irqLine2D);
            printk("  registerMemBase2D = 0x%08lX\n", registerMemBase2D);
            printk("  registerMemSize2D = 0x%08lX\n", registerMemSize2D);
        }

        printk("  contiguousSize    = %ld\n",     contiguousSize);
        printk("  contiguousBase    = 0x%08lX\n", contiguousBase);
        printk("  bankSize          = 0x%08lX\n", bankSize);
        printk("  fastClear         = %d\n",      fastClear);
        printk("  compression       = %d\n",      compression);
        printk("  signal            = %d\n",      signal);
        printk("  baseAddress       = 0x%08lX\n", baseAddress);
        printk("  physSize          = 0x%08lX\n", physSize);
    }

    /* Create the GAL device. */
    gcmkONERROR(gckGALDEVICE_Construct(
        dev, clk,
        irqLine,
        registerMemBase, registerMemSize,
        irqLine2D,
        registerMemBase2D, registerMemSize2D,
        contiguousBase, contiguousSize,
        bankSize, fastClear, compression, baseAddress, physSize, signal,
        &device
        ));

    /* Start the GAL device. */
    gcmkONERROR(gckGALDEVICE_Start(device));

    if ((physSize != 0)
       && (device->kernels[gcvCORE_MAJOR] != NULL)
       && (device->kernels[gcvCORE_MAJOR]->hardware->mmuVersion != 0))
    {
        status = gckMMU_Enable(device->kernels[gcvCORE_MAJOR]->mmu, baseAddress, physSize);
        gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
            "Enable new MMU: status=%d\n", status);

        if ((device->kernels[gcvCORE_2D] != NULL)
            && (device->kernels[gcvCORE_2D]->hardware->mmuVersion != 0))
        {
            status = gckMMU_Enable(device->kernels[gcvCORE_2D]->mmu, baseAddress, physSize);
            gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                "Enable new MMU for 2D: status=%d\n", status);
        }

        /* Reset the base address */
        device->baseAddress = 0;
    }

    /* Register the character device. */
    ret = register_chrdev(major, DRV_NAME, &driver_fops);

    if (ret < 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not allocate major number for mmap.\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_OUT_OF_MEMORY);
    }

    if (major == 0)
    {
        major = ret;
    }

    /* Create the device class. */
    device_class = class_create(THIS_MODULE, "graphics_class");

    if (IS_ERR(device_class))
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Failed to create the class.\n",
            __FUNCTION__, __LINE__
            );

        gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
    }

    device_create(device_class, NULL, MKDEV(major, 0), NULL, "galcore");

    galDevice = device;
    gpuClass  = device_class;

    gcmkTRACE_ZONE(
        gcvLEVEL_INFO, gcvZONE_DRIVER,
        "%s(%d): irqLine=%d, contiguousSize=%lu, memBase=0x%lX\n",
        __FUNCTION__, __LINE__,
        irqLine, contiguousSize, registerMemBase
        );

    /* Success. */
    gcmkFOOTER_NO();
    return 0;

OnError:
    if (!IS_ERR(clk))
    {
        clk_disable_unprepare(clk);
        clk_put(clk);
    }

    /* Roll back. */
    if (device_class != NULL)
    {
        device_destroy(device_class, MKDEV(major, 0));
        class_destroy(device_class);
    }

    if (device != NULL)
    {
        gcmkVERIFY_OK(gckGALDEVICE_Stop(device));
        gcmkVERIFY_OK(gckGALDEVICE_Destroy(device));
    }

    gcmkFOOTER();
    return result;
}

#if !USE_PLATFORM_DRIVER
static void __exit drv_exit(void)
#else
static void drv_exit(void)
#endif
{
#ifndef CONFIG_MACH_INGENIC
    struct clk *clk = galDevice->clk;
#endif

    gcmkHEADER();

    gcmkASSERT(gpuClass != NULL);
    device_destroy(gpuClass, MKDEV(major, 0));
    class_destroy(gpuClass);

    unregister_chrdev(major, DRV_NAME);

    gcmkVERIFY_OK(gckGALDEVICE_Stop(galDevice));
    gcmkVERIFY_OK(gckGALDEVICE_Destroy(galDevice));

#ifndef CONFIG_MACH_INGENIC
    clk_disable_unprepare(clk);
    clk_put(clk);
#endif

    gcmkFOOTER_NO();
}

#if !USE_PLATFORM_DRIVER
    module_init(drv_init);
    module_exit(drv_exit);
#else

static int gpu_probe(struct platform_device *pdev)
{
    int ret = -ENODEV;
    struct resource* res;

    gcmkHEADER();

    res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);

    if (!res)
    {
        printk(KERN_ERR "%s: No irq line supplied.\n",__FUNCTION__);
        goto gpu_probe_fail;
    }

    irqLine = res->start;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

    if (!res)
    {
        printk(KERN_ERR "%s: No register base supplied.\n",__FUNCTION__);
        goto gpu_probe_fail;
    }

    registerMemBase = res->start;
    registerMemSize = res->end - res->start + 1;

    res = platform_get_resource(pdev, IORESOURCE_DMA, 0);

    if (res)
    {
        contiguousBase = res->start;
        contiguousSize = res->end - res->start + 1;
    }
    else
    {
        printk(KERN_INFO "%s: Auto-configuring video memory.\n",__FUNCTION__);
        contiguousBase = 0;
        contiguousSize = 0x400000;
    }

    dev_info(&pdev->dev, "driver v4.6.6, initializing\n");

    ret = drv_init(&pdev->dev);
    if (ret)
    {
        goto gpu_probe_fail;
    }

    platform_set_drvdata(pdev, galDevice);

    dev_info(&pdev->dev, "GPU initialized, clocked at %luMHz\n",
             clk_get_rate(galDevice->clk) / 1000000);

    //galDevice->clk_enabled = 0;
    //clk_disable(galDevice->clk);

    gcmkFOOTER_NO();
    return 0;

gpu_probe_fail:
    gcmkFOOTER_ARG(KERN_INFO "Failed to register gpu driver: %d\n", ret);
    return ret;
}

static int gpu_remove(struct platform_device *pdev)
{
    gcmkHEADER();
    drv_exit();
    gcmkFOOTER_NO();
    return 0;
}

static int gpu_suspend(struct platform_device *dev, pm_message_t state)
{
    gceSTATUS status;
    gckGALDEVICE device;
    int i;

    device = platform_get_drvdata(dev);

    clk_disable(device->clk);

    for (i = 0; i < gcdCORE_COUNT; i++)
    {
        if (device->kernels[i] != NULL)
        {
            /* Store states. */
            status = gckHARDWARE_QueryPowerManagementState(device->kernels[i]->hardware, &device->statesStored[i]);
            if (gcmIS_ERROR(status))
            {
                return -1;
            }

            status = gckHARDWARE_SetPowerManagementState(device->kernels[i]->hardware, gcvPOWER_OFF);
            if (gcmIS_ERROR(status))
            {
                return -1;
            }
        }
    }

    return 0;
}

static int gpu_resume(struct platform_device *dev)
{
    gceSTATUS status;
    gckGALDEVICE device;
    int i;
    gceCHIPPOWERSTATE   statesStored;

    device = platform_get_drvdata(dev);

    clk_enable(device->clk);

    for (i = 0; i < gcdCORE_COUNT; i++)
    {
        if (device->kernels[i] != NULL)
        {
            status = gckHARDWARE_SetPowerManagementState(device->kernels[i]->hardware, gcvPOWER_ON);
            if (gcmIS_ERROR(status))
            {
                return -1;
            }

            /* Convert global state to crossponding internal state. */
            switch(device->statesStored[i])
            {
            case gcvPOWER_OFF:
                statesStored = gcvPOWER_OFF_BROADCAST;
                break;
            case gcvPOWER_IDLE:
                statesStored = gcvPOWER_IDLE_BROADCAST;
                break;
            case gcvPOWER_SUSPEND:
                statesStored = gcvPOWER_SUSPEND_BROADCAST;
                break;
            case gcvPOWER_ON:
                statesStored = gcvPOWER_ON_AUTO;
                break;
            default:
                statesStored = device->statesStored[i];
                break;
            }

            /* Restore states. */
            status = gckHARDWARE_SetPowerManagementState(device->kernels[i]->hardware, statesStored);
            if (gcmIS_ERROR(status))
            {
                return -1;
            }
        }
    }

    return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id gpu_dt_ids[] = {
	{ .compatible = "ingenic,jz4770-gpu-subsystem", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gpu_dt_ids);
#endif

static struct platform_driver gpu_driver = {
    .probe      = gpu_probe,
    .remove     = gpu_remove,

    .suspend    = gpu_suspend,
    .resume     = gpu_resume,

    .driver     = {
        .name           = DEVICE_NAME,
        .of_match_table = of_match_ptr(gpu_dt_ids),
    }
};

static int __init gpu_init(void)
{
    int ret = 0;

    ret = platform_driver_register(&gpu_driver);
    return ret;
}

static void __exit gpu_exit(void)
{
    platform_driver_unregister(&gpu_driver);
}

module_init(gpu_init);
module_exit(gpu_exit);

#endif
