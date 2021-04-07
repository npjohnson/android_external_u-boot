/****************************************************************************
*
*		     Video BOOT Graphics Card POST Module
*
*  ========================================================================
*   Copyright (C) 2007 Freescale Semiconductor, Inc.
*   Jason Jin <Jason.jin@freescale.com>
*
*   Copyright (C) 1991-2004 SciTech Software, Inc. All rights reserved.
*
*   This file may be distributed and/or modified under the terms of the
*   GNU General Public License version 2.0 as published by the Free
*   Software Foundation and appearing in the file LICENSE.GPL included
*   in the packaging of this file.
*
*   Licensees holding a valid Commercial License for this product from
*   SciTech Software, Inc. may use this file in accordance with the
*   Commercial License Agreement provided with the Software.
*
*   This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING
*   THE WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
*   PURPOSE.
*
*   See http://www.scitechsoft.com/license/ for information about
*   the licensing options available and how to purchase a Commercial
*   License Agreement.
*
*   Contact license@scitechsoft.com if any conditions of this licensing
*   are not clear to you, or you have questions about licensing options.
*
*  ========================================================================
*
* Language:	ANSI C
* Environment:	Linux Kernel
* Developer:	Kendall Bennett
*
* Description:	Module to implement booting PCI/AGP controllers on the
*		bus. We use the x86 real mode emulator to run the BIOS on
*		graphics controllers to bring the cards up.
*
*		Note that at present this module does *not* support
*		multiple controllers.
*
*		The orignal name of this file is warmboot.c.
*		Jason ported this file to u-boot to run the ATI video card
*		BIOS in u-boot.
****************************************************************************/
#include <common.h>
#include <bios_emul.h>
#include <errno.h>
#include <malloc.h>
#include <vbe.h>
#include "biosemui.h"

/* Length of the BIOS image */
#define MAX_BIOSLEN	    (128 * 1024L)

/* Place to save PCI BAR's that we change and later restore */
static u32 saveROMBaseAddress;
static u32 saveBaseAddress10;
static u32 saveBaseAddress14;
static u32 saveBaseAddress18;
static u32 saveBaseAddress20;

static void atibios_set_vesa_mode(RMREGS *regs, int vesa_mode,
				  struct vbe_mode_info *mode_info)
{
	debug("VBE: Setting VESA mode %#04x\n", vesa_mode);
	/* request linear framebuffer mode */
	vesa_mode |= (1 << 14);
	/* request clearing of framebuffer */
	vesa_mode &= ~(1 << 15);
	regs->e.eax = VESA_SET_MODE;
	regs->e.ebx = vesa_mode;
	BE_int86(0x10, regs, regs);

	int offset = 0x2000;
	void *buffer = (void *)(M.mem_base + offset);

	u16 buffer_seg = (((unsigned long)offset) >> 4) & 0xff00;
	u16 buffer_adr = ((unsigned long)offset) & 0xffff;
	regs->e.eax = VESA_GET_MODE_INFO;
	regs->e.ebx = 0;
	regs->e.ecx = vesa_mode;
	regs->e.edx = 0;
	regs->e.esi = buffer_seg;
	regs->e.edi = buffer_adr;
	BE_int86(0x10, regs, regs);
	memcpy(mode_info->mode_info_block, buffer,
	       sizeof(struct vbe_mode_info));
	mode_info->valid = true;

	vesa_mode |= (1 << 14);
	/* request clearing of framebuffer */
	vesa_mode &= ~(1 << 15);
	regs->e.eax = VESA_SET_MODE;
	regs->e.ebx = vesa_mode;
	BE_int86(0x10, regs, regs);
}

/****************************************************************************
PARAMETERS:
pcidev	- PCI device info for the video card on the bus to boot
vga_info - BIOS emulator VGA info structure

REMARKS:
This function executes the BIOS POST code on the controller. We assume that
at this stage the controller has its I/O and memory space enabled and
that all other controllers are in a disabled state.
****************************************************************************/
static void PCI_doBIOSPOST(pci_dev_t pcidev, BE_VGAInfo *vga_info,
			   int vesa_mode, struct vbe_mode_info *mode_info)
{
	RMREGS regs;
	RMSREGS sregs;

	/* Determine the value to store in AX for BIOS POST. Per the PCI specs,
	 AH must contain the bus and AL must contain the devfn, encoded as
	 (dev << 3) | fn
	 */
	memset(&regs, 0, sizeof(regs));
	memset(&sregs, 0, sizeof(sregs));
	regs.x.ax = ((int)PCI_BUS(pcidev) << 8) |
	    ((int)PCI_DEV(pcidev) << 3) | (int)PCI_FUNC(pcidev);

	/*Setup the X86 emulator for the VGA BIOS*/
	BE_setVGA(vga_info);

	/*Execute the BIOS POST code*/
	BE_callRealMode(0xC000, 0x0003, &regs, &sregs);

	/*Cleanup and exit*/
	BE_getVGA(vga_info);

	if (vesa_mode != -1)
		atibios_set_vesa_mode(&regs, vesa_mode, mode_info);
}

/****************************************************************************
PARAMETERS:
pcidev	- PCI device info for the video card on the bus
bar	- Place to return the base address register offset to use

RETURNS:
The address to use to map the secondary BIOS (AGP devices)

REMARKS:
Searches all the PCI base address registers for the device looking for a
memory mapping that is large enough to hold our ROM BIOS. We usually end up
finding the framebuffer mapping (usually BAR 0x10), and we use this mapping
to map the BIOS for the device into. We use a mapping that is already
assigned to the device to ensure the memory range will be passed through
by any PCI->PCI or AGP->PCI bridge that may be present.

NOTE: Usually this function is only used for AGP devices, but it may be
      used for PCI devices that have already been POST'ed and the BIOS
      ROM base address has been zero'ed out.

NOTE: This function leaves the original memory aperture disabled by leaving
      it programmed to all 1's. It must be restored to the correct value
      later.
****************************************************************************/
static u32 PCI_findBIOSAddr(pci_dev_t pcidev, int *bar)
{
	u32 base, size;

	for (*bar = 0x10; *bar <= 0x14; (*bar) += 4) {
		pci_read_config_dword(pcidev, *bar, &base);
		if (!(base & 0x1)) {
			pci_write_config_dword(pcidev, *bar, 0xFFFFFFFF);
			pci_read_config_dword(pcidev, *bar, &size);
			size = ~(size & ~0xFF) + 1;
			if (size >= MAX_BIOSLEN)
				return base & ~0xFF;
		}
	}
	return 0;
}

/****************************************************************************
REMARKS:
Some non-x86 Linux kernels map PCI relocateable I/O to values that
are above 64K, which will not work with the BIOS image that requires
the offset for the I/O ports to be a maximum of 16-bits. Ideally
someone should fix the kernel to map the I/O ports for VGA compatible
devices to a different location (or just all I/O ports since it is
unlikely you can have enough devices in the machine to use up all
64K of the I/O space - a total of more than 256 cards would be
necessary).

Anyway to fix this we change all I/O mapped base registers and
chop off the top bits.
****************************************************************************/
static void PCI_fixupIObase(pci_dev_t pcidev, int reg, u32 * base)
{
	if ((*base & 0x1) && (*base > 0xFFFE)) {
		*base &= 0xFFFF;
		pci_write_config_dword(pcidev, reg, *base);

	}
}

/****************************************************************************
PARAMETERS:
pcidev	- PCI device info for the video card on the bus

RETURNS:
Pointers to the mapped BIOS image

REMARKS:
Maps a pointer to the BIOS image on the graphics card on the PCI bus.
****************************************************************************/
void *PCI_mapBIOSImage(pci_dev_t pcidev)
{
	u32 BIOSImageBus;
	int BIOSImageBAR;
	u8 *BIOSImage;

	/*Save PCI BAR registers that might get changed*/
	pci_read_config_dword(pcidev, PCI_ROM_ADDRESS, &saveROMBaseAddress);
	pci_read_config_dword(pcidev, PCI_BASE_ADDRESS_0, &saveBaseAddress10);
	pci_read_config_dword(pcidev, PCI_BASE_ADDRESS_1, &saveBaseAddress14);
	pci_read_config_dword(pcidev, PCI_BASE_ADDRESS_2, &saveBaseAddress18);
	pci_read_config_dword(pcidev, PCI_BASE_ADDRESS_4, &saveBaseAddress20);

	/*Fix up I/O base registers to less than 64K */
	if(saveBaseAddress14 != 0)
		PCI_fixupIObase(pcidev, PCI_BASE_ADDRESS_1, &saveBaseAddress14);
	else
		PCI_fixupIObase(pcidev, PCI_BASE_ADDRESS_4, &saveBaseAddress20);

	/* Some cards have problems that stop us from being able to read the
	 BIOS image from the ROM BAR. To fix this we have to do some chipset
	 specific programming for different cards to solve this problem.
	*/

	BIOSImageBus = PCI_findBIOSAddr(pcidev, &BIOSImageBAR);
	if (BIOSImageBus == 0) {
		printf("Find bios addr error\n");
		return NULL;
	}

	BIOSImage = pci_bus_to_virt(pcidev, BIOSImageBus,
				    PCI_REGION_MEM, 0, MAP_NOCACHE);

	/*Change the PCI BAR registers to map it onto the bus.*/
	pci_write_config_dword(pcidev, BIOSImageBAR, 0);
	pci_write_config_dword(pcidev, PCI_ROM_ADDRESS, BIOSImageBus | 0x1);

	udelay(1);

	/*Check that the BIOS image is valid. If not fail, or return the
	 compiled in BIOS image if that option was enabled
	 */
	if (BIOSImage[0] != 0x55 || BIOSImage[1] != 0xAA || BIOSImage[2] == 0) {
		return NULL;
	}

	return BIOSImage;
}

/****************************************************************************
PARAMETERS:
pcidev	- PCI device info for the video card on the bus

REMARKS:
Unmaps the BIOS image for the device and restores framebuffer mappings
****************************************************************************/
void PCI_unmapBIOSImage(pci_dev_t pcidev, void *BIOSImage)
{
	pci_write_config_dword(pcidev, PCI_ROM_ADDRESS, saveROMBaseAddress);
	pci_write_config_dword(pcidev, PCI_BASE_ADDRESS_0, saveBaseAddress10);
	pci_write_config_dword(pcidev, PCI_BASE_ADDRESS_1, saveBaseAddress14);
	pci_write_config_dword(pcidev, PCI_BASE_ADDRESS_2, saveBaseAddress18);
	pci_write_config_dword(pcidev, PCI_BASE_ADDRESS_4, saveBaseAddress20);
}

/****************************************************************************
PARAMETERS:
pcidev	- PCI device info for the video card on the bus to boot
VGAInfo - BIOS emulator VGA info structure

RETURNS:
true if successfully initialised, false if not.

REMARKS:
Loads and POST's the display controllers BIOS, directly from the BIOS
image we can extract over the PCI bus.
****************************************************************************/
static int PCI_postController(pci_dev_t pcidev, uchar *bios_rom, int bios_len,
			      BE_VGAInfo *vga_info, int vesa_mode,
			      struct vbe_mode_info *mode_info)
{
	u32 bios_image_len;
	uchar *mapped_bios;
	uchar *copy_of_bios;

	if (bios_rom) {
		copy_of_bios = bios_rom;
		bios_image_len = bios_len;
	} else {
		/*
		 * Allocate memory to store copy of BIOS from display
		 * controller
		 */
		mapped_bios = PCI_mapBIOSImage(pcidev);
		if (mapped_bios == NULL) {
			printf("videoboot: Video ROM failed to map!\n");
			return false;
		}

		bios_image_len = mapped_bios[2] * 512;

		copy_of_bios = malloc(bios_image_len);
		if (copy_of_bios == NULL) {
			printf("videoboot: Out of memory!\n");
			return false;
		}
		memcpy(copy_of_bios, mapped_bios, bios_image_len);
		PCI_unmapBIOSImage(pcidev, mapped_bios);
	}

	/*Save information in vga_info structure*/
	vga_info->function = PCI_FUNC(pcidev);
	vga_info->device = PCI_DEV(pcidev);
	vga_info->bus = PCI_BUS(pcidev);
	vga_info->pcidev = pcidev;
	vga_info->BIOSImage = copy_of_bios;
	vga_info->BIOSImageLen = bios_image_len;

	/*Now execute the BIOS POST for the device*/
	if (copy_of_bios[0] != 0x55 || copy_of_bios[1] != 0xAA) {
		printf("videoboot: Video ROM image is invalid!\n");
		return false;
	}

	PCI_doBIOSPOST(pcidev, vga_info, vesa_mode, mode_info);

	/*Reset the size of the BIOS image to the final size*/
	vga_info->BIOSImageLen = copy_of_bios[2] * 512;
	return true;
}

int biosemu_setup(pci_dev_t pcidev, BE_VGAInfo **vga_infop)
{
	BE_VGAInfo *VGAInfo;

	printf("videoboot: Booting PCI video card bus %d, function %d, device %d\n",
	     PCI_BUS(pcidev), PCI_FUNC(pcidev), PCI_DEV(pcidev));

	/*Initialise the x86 BIOS emulator*/
	if ((VGAInfo = malloc(sizeof(*VGAInfo))) == NULL) {
		printf("videoboot: Out of memory!\n");
		return -ENOMEM;
	}
	memset(VGAInfo, 0, sizeof(*VGAInfo));
	BE_init(0, 65536, VGAInfo, 0);
	*vga_infop = VGAInfo;

	return 0;
}

void biosemu_set_interrupt_handler(int intnum, int (*int_func)(void))
{
	X86EMU_setupIntrFunc(intnum, (X86EMU_intrFuncs)int_func);
}

int biosemu_run(pci_dev_t pcidev, uchar *bios_rom, int bios_len,
		BE_VGAInfo *vga_info, int clean_up, int vesa_mode,
		struct vbe_mode_info *mode_info)
{
	/*Post all the display controller BIOS'es*/
	if (!PCI_postController(pcidev, bios_rom, bios_len, vga_info,
				vesa_mode, mode_info))
		return -EINVAL;

	/*
	 * Cleanup and exit the emulator if requested. If the BIOS emulator
	 * is needed after booting the card, we will not call BE_exit and
	 * leave it enabled for further use (ie: VESA driver etc).
	*/
	if (clean_up) {
		BE_exit();
		if (vga_info->BIOSImage)
			free(vga_info->BIOSImage);
		free(vga_info);
		vga_info = NULL;
	}

	return 0;
}

/****************************************************************************
PARAMETERS:
pcidev	    - PCI device info for the video card on the bus to boot
pVGAInfo    - Place to return VGA info structure is requested
cleanUp	    - true to clean up on exit, false to leave emulator active

REMARKS:
Boots the PCI/AGP video card on the bus using the Video ROM BIOS image
and the X86 BIOS emulator module.
****************************************************************************/
int BootVideoCardBIOS(pci_dev_t pcidev, BE_VGAInfo **pVGAInfo, int clean_up)
{
	BE_VGAInfo *VGAInfo;
	int ret;

	ret = biosemu_setup(pcidev, &VGAInfo);
	if (ret)
		return false;
	ret = biosemu_run(pcidev, NULL, 0, VGAInfo, clean_up, -1, NULL);
	if (ret)
		return false;

	/* Return VGA info pointer if the caller requested it*/
	if (pVGAInfo)
		*pVGAInfo = VGAInfo;

	return true;
}
