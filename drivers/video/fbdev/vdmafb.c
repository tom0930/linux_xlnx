/*
 * AXI VDMA frame buffer driver
 * Based on: ocfb.c, simplefb.c, axi_hdmi_crtc.c
 *
 * Copyright (C) 2014 Topic Embedded Products
 * Author: Mike Looijmans <mike.looijmans@topic.nl>
 *
 * Licensed under the GPL-2.
 *
 * Example devicetree contents:
 * axi_vdma_0: dma@43000000 {
 *         compatible = "xlnx,axi-vdma-1.00.a";
 *         #dma-cells = <1>;
 *         reg = <0x43000000 0x10000>;
 *         interrupt-parent = <&intc>;
 *         xlnx,num-fstores = <0x1>;
 *         xlnx,flush-fsync = <0x2>;
 *         xlnx,addrwidth = <0x20>;
 *         clocks = <&clkc 0>;
 *         clock-names = "s_axi_lite_aclk";
 *         dma-channel@43000000 {
 *                 compatible = "xlnx,axi-vdma-mm2s-channel";
 *                 interrupts = <0 33 4>;
 *                 xlnx,datawidth = <0x20>;
 *                 xlnx,device-id = <0x0>;
 *         } ;
 * };
 * axi_vdma_lcd {
 *         compatible = "topic,vdma-fb";
 *         dmas = <&axi_vdma_0 0>;
 *         dma-names = "axivdma";
 * };
 *
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/dma/xilinx_dma.h>

struct vdmafb_dev {
        struct fb_info info;
        /* Physical and virtual addresses of framebuffer */
        phys_addr_t fb_phys;
        void __iomem *fb_virt;
        /* VDMA handle */
        struct dma_chan *dma;
        struct dma_interleaved_template *dma_template;
        /* Palette data */
        u32 pseudo_palette[16];
};

static int vdmafb_setupfb(struct vdmafb_dev *fbdev)
{
        struct fb_var_screeninfo *var = &fbdev->info.var;
        struct dma_async_tx_descriptor *desc;
        struct dma_interleaved_template *dma_template = fbdev->dma_template;
        struct xilinx_vdma_config vdma_config;
        int hsize = var->xres * 4;
        int ret;

        dmaengine_terminate_all(fbdev->dma);

        /* Setup VDMA address etc */
        memset(&vdma_config, 0, sizeof(vdma_config));
        vdma_config.park = 1;
        xilinx_vdma_channel_set_config(fbdev->dma, &vdma_config);

       /*
        * Interleaved DMA:
        * Each interleaved frame is a row (hsize) implemented in ONE
        * chunk (sgl has len 1).
        * The number of interleaved frames is the number of rows (vsize).
        * The icg in used to pack data to the HW, so that the buffer len
        * is fb->piches[0], but the actual size for the hw is somewhat less
        */
       dma_template->dir = DMA_MEM_TO_DEV;
       dma_template->src_start = fbdev->fb_phys;
       /* sgl list have just one entry (each interleaved frame have 1 chunk) */
       dma_template->frame_size = 1;
       /* the number of interleaved frame, each has the size specified in sgl */
       dma_template->numf = var->yres;
       dma_template->src_sgl = 1;
       dma_template->src_inc = 1;
       /* vdma IP does not provide any addr to the hdmi IP */
       dma_template->dst_inc = 0;
       dma_template->dst_sgl = 0;
       /* horizontal size */
       dma_template->sgl[0].size = hsize;
       /* the vdma driver seems to look at icg, and not src_icg */
       dma_template->sgl[0].icg = 0; /*  stride - hsize */

       desc = dmaengine_prep_interleaved_dma(fbdev->dma, dma_template, 0);
        if (!desc) {
                pr_err("Failed to prepare DMA descriptor\n");
                return -ENOMEM;
        } else {
                dmaengine_submit(desc);
                dma_async_issue_pending(fbdev->dma);
        }

        return 0;
}

static void vdmafb_init_fix(struct vdmafb_dev *fbdev)
{
        struct fb_var_screeninfo *var = &fbdev->info.var;
        struct fb_fix_screeninfo *fix = &fbdev->info.fix;

        strcpy(fix->id, "vdma-fb");
        fix->line_length = var->xres * (var->bits_per_pixel/8);
        fix->smem_len = fix->line_length * var->yres;
        fix->type = FB_TYPE_PACKED_PIXELS;
        fix->visual = FB_VISUAL_TRUECOLOR;
}

static void vdmafb_init_var(struct vdmafb_dev *fbdev, struct platform_device *pdev)
{
        struct device_node *np = pdev->dev.of_node;
        struct fb_var_screeninfo *var = &fbdev->info.var;
        int ret;

        var->xres = 800;
        var->yres = 480;

        var->accel_flags = FB_ACCEL_NONE;
        var->activate = FB_ACTIVATE_NOW;
        var->xres_virtual = var->xres;
        var->yres_virtual = var->yres;
        var->bits_per_pixel = 32;
        /* Clock settings */
        var->pixclock = KHZ2PICOS(33260);
        var->vmode = FB_VMODE_NONINTERLACED;
        /* 32 BPP */
        var->transp.offset = 24;
        var->transp.length = 8;
        var->red.offset = 16;
        var->red.length = 8;
        var->green.offset = 8;
        var->green.length = 8;
        var->blue.offset = 0;
        var->blue.length = 8;
}

static int vdmafb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                              u_int transp, struct fb_info *info)
{
        u32 *pal = info->pseudo_palette;
        u32 cr = red >> (16 - info->var.red.length);
        u32 cg = green >> (16 - info->var.green.length);
        u32 cb = blue >> (16 - info->var.blue.length);
        u32 value;

        if (regno >= 16)
                return -EINVAL;

        value = (cr << info->var.red.offset) |
                (cg << info->var.green.offset) |
                (cb << info->var.blue.offset);
        if (info->var.transp.length > 0) {
                u32 mask = (1 << info->var.transp.length) - 1;
                mask <<= info->var.transp.offset;
                value |= mask;
        }
        pal[regno] = value;

        return 0;
}

static struct fb_ops vdmafb_ops = {
        .owner		= THIS_MODULE,
        .fb_setcolreg	= vdmafb_setcolreg,
        .fb_fillrect	= sys_fillrect,
        .fb_copyarea	= sys_copyarea,
        .fb_imageblit	= sys_imageblit,
};

static int vdmafb_probe(struct platform_device *pdev)
{
        int ret = 0;
        struct vdmafb_dev *fbdev;
        struct resource *res;
        int fbsize;

        fbdev = devm_kzalloc(&pdev->dev, sizeof(*fbdev), GFP_KERNEL);
        if (!fbdev)
                return -ENOMEM;

        platform_set_drvdata(pdev, fbdev);

        fbdev->info.fbops = &vdmafb_ops;
        fbdev->info.device = &pdev->dev;
        fbdev->info.par = fbdev;

        fbdev->dma_template = devm_kzalloc(&pdev->dev,
                sizeof(struct dma_interleaved_template) +
                sizeof(struct data_chunk), GFP_KERNEL);
        if (!fbdev->dma_template)
                return -ENOMEM;

        vdmafb_init_var(fbdev, pdev);
        vdmafb_init_fix(fbdev);

        /* Allocate framebuffer memory */
        fbsize = fbdev->info.fix.smem_len;
        fbdev->fb_virt = dma_alloc_coherent(&pdev->dev, PAGE_ALIGN(fbsize),
                                            &fbdev->fb_phys, GFP_KERNEL);
        if (!fbdev->fb_virt) {
                dev_err(&pdev->dev,
                        "Frame buffer memory allocation failed\n");
                return -ENOMEM;
        }
        fbdev->info.fix.smem_start = fbdev->fb_phys;
        fbdev->info.screen_base = fbdev->fb_virt;
        fbdev->info.pseudo_palette = fbdev->pseudo_palette;

        pr_debug("%s virt=%p phys=%x size=%d\n", __func__,
                fbdev->fb_virt, fbdev->fb_phys, fbsize);

        /* Clear framebuffer */
        memset_io(fbdev->fb_virt, 0, fbsize);

	pr_info("vdma is load over");

        fbdev->dma = dma_request_slave_channel(&pdev->dev, "axivdma");
        if (IS_ERR_OR_NULL(fbdev->dma)) {
                ret = PTR_ERR(fbdev->dma);
                dev_err(&pdev->dev, "Failed to allocate DMA channel (%d).\n", ret);
                goto err_dma_free;
        }

        /* Setup and enable the framebuffer */
        vdmafb_setupfb(fbdev);
        

        ret = fb_alloc_cmap(&fbdev->info.cmap, 256, 0);
        if (ret) {
                dev_err(&pdev->dev, "fb_alloc_cmap failed\n");
        }

        /* Register framebuffer */
        ret = register_framebuffer(&fbdev->info);
        if (ret) {
                dev_err(&pdev->dev, "Framebuffer registration failed\n");
                goto err_channel_free;
        }
        pr_info("vdma is load over");

        return 0;

err_channel_free:
        dma_release_channel(fbdev->dma);
err_dma_free:
        dma_free_coherent(&pdev->dev, PAGE_ALIGN(fbsize), fbdev->fb_virt,
                          fbdev->fb_phys);

        return ret;
}

static int vdmafb_remove(struct platform_device *pdev)
{
        struct vdmafb_dev *fbdev = platform_get_drvdata(pdev);

        unregister_framebuffer(&fbdev->info);

        dma_release_channel(fbdev->dma);
        dma_free_coherent(&pdev->dev, PAGE_ALIGN(fbdev->info.fix.smem_len),
                          fbdev->fb_virt, fbdev->fb_phys);
        fb_dealloc_cmap(&fbdev->info.cmap);
        return 0;
}

static struct of_device_id vdmafb_match[] = {
        { .compatible = "topic,vdma-fb", },
        {},
};
MODULE_DEVICE_TABLE(of, vdmafb_match);

static struct platform_driver vdmafb_driver = {
        .probe  = vdmafb_probe,
        .remove	= vdmafb_remove,
        .driver = {
                .name = "vdmafb_fb",
                .of_match_table = vdmafb_match,
        }
};
module_platform_driver(vdmafb_driver);

MODULE_AUTHOR("Mike Looijmans <mike.looijmans@topic.nl>");
MODULE_DESCRIPTION("Driver for VDMA controlled framebuffer");
MODULE_LICENSE("GPL v2");
