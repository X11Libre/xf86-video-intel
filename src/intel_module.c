/*
 * Copyright 1998-1999 Precision Insight, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>
#include <xorgVersion.h>

#include <xf86.h>
#include <xf86Parser.h>

#if XORG_VERSION_CURRENT < XORG_VERSION_NUMERIC(1,6,99,0,0)
#include <xf86Resources.h>
#endif

#include "intel_driver.h"
#include "intel_options.h"
#include "legacy/legacy.h"
#include "sna/sna_module.h"
#include "uxa/uxa_module.h"

#include "i915_pciids.h" /* copied from (kernel) include/drm/i915_pciids.h */

#ifdef XSERVER_PLATFORM_BUS
#include <xf86platformBus.h>
#endif

#ifndef XF86_ALLOCATE_GPU_SCREEN
#define XF86_ALLOCATE_GPU_SCREEN 0
#endif

static const struct intel_device_info intel_generic_info = {
	.gen = -1,
};

static const struct intel_device_info intel_i81x_info = {
	.gen = 010,
};

static const struct intel_device_info intel_i830_info = {
	.gen = 020,
};
static const struct intel_device_info intel_i845_info = {
	.gen = 020,
};
static const struct intel_device_info intel_i855_info = {
	.gen = 021,
};
static const struct intel_device_info intel_i865_info = {
	.gen = 022,
};

static const struct intel_device_info intel_i915_info = {
	.gen = 030,
};
static const struct intel_device_info intel_i945_info = {
	.gen = 031,
};

static const struct intel_device_info intel_g33_info = {
	.gen = 033,
};

static const struct intel_device_info intel_i965_info = {
	.gen = 040,
};

static const struct intel_device_info intel_g4x_info = {
	.gen = 045,
};

static const struct intel_device_info intel_ironlake_info = {
	.gen = 050,
};

static const struct intel_device_info intel_sandybridge_info = {
	.gen = 060,
};

static const struct intel_device_info intel_ivybridge_info = {
	.gen = 070,
};

static const struct intel_device_info intel_valleyview_info = {
	.gen = 071,
};

static const struct intel_device_info intel_haswell_info = {
	.gen = 075,
};

static const struct intel_device_info intel_broadwell_info = {
	.gen = 0100,
};

static const struct intel_device_info intel_cherryview_info = {
	.gen = 0101,
};

static const struct intel_device_info intel_skylake_info = {
	.gen = 0110,
};

static const struct intel_device_info intel_broxton_info = {
	.gen = 0111,
};

static const struct intel_device_info intel_kabylake_info = {
	.gen = 0112,
};

static const struct intel_device_info intel_geminilake_info = {
	.gen = 0113,
};

static const struct intel_device_info intel_coffeelake_info = {
	.gen = 0114,
};

static const struct intel_device_info intel_cannonlake_info = {
	.gen = 0120,
};

static const struct intel_device_info intel_icelake_info = {
	.gen = 0130,
};

static const struct intel_device_info intel_elkhartlake_info = {
	.gen = 0131,
};

static const struct intel_device_info intel_tigerlake_info = {
	.gen = 0140,
};

static const SymTabRec intel_chipsets[] = {
	{PCI_CHIP_I810,				"i810"},
	{PCI_CHIP_I810_DC100,			"i810-dc100"},
	{PCI_CHIP_I810_E,			"i810e"},
	{PCI_CHIP_I815,				"i815"},
	{PCI_CHIP_I830_M,			"i830M"},
	{PCI_CHIP_845_G,			"845G"},
	{PCI_CHIP_I854,				"854"},
	{PCI_CHIP_I855_GM,			"852GM/855GM"},
	{PCI_CHIP_I865_G,			"865G"},
	{PCI_CHIP_I915_G,			"915G"},
	{PCI_CHIP_E7221_G,			"E7221 (i915)"},
	{PCI_CHIP_I915_GM,			"915GM"},
	{PCI_CHIP_I945_G,			"945G"},
	{PCI_CHIP_I945_GM,			"945GM"},
	{PCI_CHIP_I945_GME,			"945GME"},
	{PCI_CHIP_PINEVIEW_M,			"Pineview GM"},
	{PCI_CHIP_PINEVIEW_G,			"Pineview G"},
	{PCI_CHIP_I965_G,			"965G"},
	{PCI_CHIP_G35_G,			"G35"},
	{PCI_CHIP_I965_Q,			"965Q"},
	{PCI_CHIP_I946_GZ,			"946GZ"},
	{PCI_CHIP_I965_GM,			"965GM"},
	{PCI_CHIP_I965_GME,			"965GME/GLE"},
	{PCI_CHIP_G33_G,			"G33"},
	{PCI_CHIP_Q35_G,			"Q35"},
	{PCI_CHIP_Q33_G,			"Q33"},
	{PCI_CHIP_GM45_GM,			"GM45"},
	{PCI_CHIP_G45_E_G,			"4 Series"},
	{PCI_CHIP_G45_G,			"G45/G43"},
	{PCI_CHIP_Q45_G,			"Q45/Q43"},
	{PCI_CHIP_G41_G,			"G41"},
	{PCI_CHIP_B43_G,			"B43"},
	{PCI_CHIP_B43_G1,			"B43"},

	{0, ""},

	{PCI_CHIP_IRONLAKE_D_G,			"HD Graphics"},
	{PCI_CHIP_IRONLAKE_M_G,			"HD Graphics"},
	{PCI_CHIP_SANDYBRIDGE_GT1,		"HD Graphics 2000" },
	{PCI_CHIP_SANDYBRIDGE_GT2,		"HD Graphics 3000" },
	{PCI_CHIP_SANDYBRIDGE_GT2_PLUS,		"HD Graphics 3000" },
	{PCI_CHIP_SANDYBRIDGE_M_GT1,		"HD Graphics 2000" },
	{PCI_CHIP_SANDYBRIDGE_M_GT2,		"HD Graphics 3000" },
	{PCI_CHIP_SANDYBRIDGE_M_GT2_PLUS,	"HD Graphics 3000" },
	{PCI_CHIP_SANDYBRIDGE_S_GT,		"HD Graphics" },
	{PCI_CHIP_IVYBRIDGE_M_GT1,		"HD Graphics 2500" },
	{PCI_CHIP_IVYBRIDGE_M_GT2,		"HD Graphics 4000" },
	{PCI_CHIP_IVYBRIDGE_D_GT1,		"HD Graphics 2500" },
	{PCI_CHIP_IVYBRIDGE_D_GT2,		"HD Graphics 4000" },
	{PCI_CHIP_IVYBRIDGE_S_GT1,		"HD Graphics" },
	{PCI_CHIP_IVYBRIDGE_S_GT2,		"HD Graphics P4000" },
	{PCI_CHIP_HASWELL_D_GT1,		"HD Graphics" },
	{PCI_CHIP_HASWELL_D_GT2,		"HD Graphics 4600" },
	{PCI_CHIP_HASWELL_D_GT3,		"HD Graphics 5000" }, /* ??? */
	{PCI_CHIP_HASWELL_M_GT1,		"HD Graphics" },
	{PCI_CHIP_HASWELL_M_GT2,		"HD Graphics 4600" },
	{PCI_CHIP_HASWELL_M_GT3,		"HD Graphics 5000" }, /* ??? */
	{PCI_CHIP_HASWELL_S_GT1,		"HD Graphics" },
	{PCI_CHIP_HASWELL_S_GT2,		"HD Graphics P4600/P4700" },
	{PCI_CHIP_HASWELL_S_GT3,		"HD Graphics 5000" }, /* ??? */
	{PCI_CHIP_HASWELL_B_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_B_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_B_GT3,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_E_GT1,		"HD Graphics" },
	{PCI_CHIP_HASWELL_E_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_E_GT3,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_ULT_D_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_ULT_D_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_ULT_D_GT3,		"Iris(TM) Graphics 5100" },
	{PCI_CHIP_HASWELL_ULT_M_GT1,		"HD Graphics" },
	{PCI_CHIP_HASWELL_ULT_M_GT2,		"HD Graphics 4400" },
	{PCI_CHIP_HASWELL_ULT_M_GT3,		"HD Graphics 5000" },
	{PCI_CHIP_HASWELL_ULT_S_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_ULT_S_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_ULT_S_GT3,		"Iris(TM) Graphics 5100" },
	{PCI_CHIP_HASWELL_ULT_B_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_ULT_B_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_ULT_B_GT3,		"Iris(TM) Graphics 5100" },
	{PCI_CHIP_HASWELL_ULT_E_GT1,		"HD Graphics" },
	{PCI_CHIP_HASWELL_ULT_E_GT2,		"HD Graphics 4200" },
	{PCI_CHIP_HASWELL_ULT_E_GT3,		"Iris(TM) Graphics 5100" },
	{PCI_CHIP_HASWELL_CRW_D_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_D_GT2,		"HD Graphics 4600" },
	{PCI_CHIP_HASWELL_CRW_D_GT3,		"Iris(TM) Pro Graphics 5200" },
	{PCI_CHIP_HASWELL_CRW_M_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_M_GT2,		"HD Graphics 4600" },
	{PCI_CHIP_HASWELL_CRW_M_GT3,		"Iris(TM) Pro Graphics 5200" },
	{PCI_CHIP_HASWELL_CRW_S_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_S_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_S_GT3,		"Iris(TM) Pro Graphics 5200" },
	{PCI_CHIP_HASWELL_CRW_B_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_B_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_B_GT3,		"Iris(TM) Pro Graphics 5200" },
	{PCI_CHIP_HASWELL_CRW_E_GT1,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_E_GT2,		"HD Graphics" }, /* ??? */
	{PCI_CHIP_HASWELL_CRW_E_GT3,		"Iris(TM) Pro Graphics 5200" },

	/* Valleyview (Baytail) */
	{0x0f30, "HD Graphics"},
	{0x0f31, "HD Graphics"},
	{0x0f32, "HD Graphics"},
	{0x0f33, "HD Graphics"},
	{0x0155, "HD Graphics"},
	{0x0157, "HD Graphics"},

	/* Broadwell Marketing names */
	{0x1602, "HD Graphics"},
	{0x1606, "HD Graphics"},
	{0x160B, "HD Graphics"},
	{0x160A, "HD Graphics"},
	{0x160D, "HD Graphics"},
	{0x160E, "HD Graphics"},
	{0x1612, "HD Graphics 5600"},
	{0x1616, "HD Graphics 5500"},
	{0x161B, "HD Graphics"},
	{0x161A, "HD Graphics"},
	{0x161D, "HD Graphics"},
	{0x161E, "HD Graphics 5300"},
	{0x1622, "Iris Pro Graphics 6200"},
	{0x1626, "HD Graphics 6000"},
	{0x162B, "Iris Graphics 6100"},
	{0x162A, "Iris Pro Graphics P6300"},
	{0x162D, "HD Graphics"},
	{0x162E, "HD Graphics"},
	{0x1632, "HD Graphics"},
	{0x1636, "HD Graphics"},
	{0x163B, "HD Graphics"},
	{0x163A, "HD Graphics"},
	{0x163D, "HD Graphics"},
	{0x163E, "HD Graphics"},

	/* Cherryview (Cherrytrail/Braswell) */
	{0x22b0, "HD Graphics"},
	{0x22b1, "HD Graphics"},
	{0x22b2, "HD Graphics"},
	{0x22b3, "HD Graphics"},

	/* Skylake */
	{0x1902, "HD Graphics 510"},
	{0x1906, "HD Graphics 510"},
	{0x190B, "HD Graphics 510"},
	{0x1912, "HD Graphics 530"},
	{0x1916, "HD Graphics 520"},
	{0x191B, "HD Graphics 530"},
	{0x191D, "HD Graphics P530"},
	{0x191E, "HD Graphics 515"},
	{0x1921, "HD Graphics 520"},
	{0x1926, "Iris Graphics 540"},
	{0x1927, "Iris Graphics 550"},
	{0x192B, "Iris Graphics 555"},
	{0x192D, "Iris Graphics P555"},
	{0x1932, "Iris Pro Graphics 580"},
	{0x193A, "Iris Pro Graphics P580"},
	{0x193B, "Iris Pro Graphics 580"},
	{0x193D, "Iris Pro Graphics P580"},

	/* Broxton (Apollolake) */
	{0x5A84, "HD Graphics 505"},
	{0x5A85, "HD Graphics 500"},

	/* Kabylake */
	{0x5916, "HD Graphics 620"},
	{0x591E, "HD Graphics 615"},

	/* Coffeelake */
	{0x3E90, "HD Graphics"},
	{0x3E93, "HD Graphics"},
	{0x3E99, "HD Graphics"},
	{0x3E91, "HD Graphics"},
	{0x3E92, "HD Graphics"},
	{0x3E96, "HD Graphics"},
	{0x3E9A, "HD Graphics"},
	{0x3E9C, "HD Graphics"},
	{0x3E9B, "HD Graphics"},
	{0x3E94, "HD Graphics"},
	{0x3E98, "HD Graphics"},
	{0x3EA1, "HD Graphics"},
	{0x3EA4, "HD Graphics"},
	{0x3EA0, "HD Graphics"},
	{0x3EA3, "HD Graphics"},
	{0x3EA9, "HD Graphics"},
	{0x3EA2, "HD Graphics"},
	{0x3EA5, "HD Graphics"},
	{0x3EA6, "HD Graphics"},
	{0x3EA7, "HD Graphics"},
	{0x3EA8, "HD Graphics"},
	{0x87CA, "HD Graphics"},
	{0x9BA5, "HD Graphics"},
	{0x9BA8, "HD Graphics"},
	{0x9BA4, "HD Graphics"},
	{0x9BA2, "HD Graphics"},
	{0x9BC5, "HD Graphics"},
	{0x9BC8, "HD Graphics"},
	{0x9BC4, "HD Graphics"},
	{0x9BC2, "HD Graphics"},
	{0x9BC6, "HD Graphics"},
	{0x9BE6, "HD Graphics"},
	{0x9BF6, "HD Graphics"},
	{0x9B21, "HD Graphics"},
	{0x9BAA, "HD Graphics"},
	{0x9BAC, "HD Graphics"},
	{0x9B41, "HD Graphics"},
	{0x9BCA, "HD Graphics"},
	{0x9BCC, "HD Graphics"},

	/* CannonLake */
	{0x5A54, "HD Graphics"},
	{0x5A5C, "HD Graphics"},
	{0x5A44, "HD Graphics"},
	{0x5A4C, "HD Graphics"},
	{0x5A51, "HD Graphics"},
	{0x5A59, "HD Graphics"},
	{0x5A41, "HD Graphics"},
	{0x5A49, "HD Graphics"},
	{0x5A52, "HD Graphics"},
	{0x5A5A, "HD Graphics"},
	{0x5A42, "HD Graphics"},
	{0x5A4A, "HD Graphics"},
	{0x5A50, "HD Graphics"},
	{0x5A40, "HD Graphics"},

	/* IceLake */
	{0x8A50, "HD Graphics"},
	{0x8A5C, "HD Graphics"},
	{0x8A59, "HD Graphics"},
	{0x8A58, "HD Graphics"},
	{0x8A52, "HD Graphics"},
	{0x8A5A, "HD Graphics"},
	{0x8A5B, "HD Graphics"},
	{0x8A57, "HD Graphics"},
	{0x8A56, "HD Graphics"},
	{0x8A71, "HD Graphics"},
	{0x8A70, "HD Graphics"},
	{0x8A53, "HD Graphics"},
	{0x8A54, "HD Graphics"},
	{0x8A51, "HD Graphics"},
	{0x8A5D, "HD Graphics"},

	/* ElkhartLake */
	{0x4500, "HD Graphics"},
	{0x4571, "HD Graphics"},
	{0x4551, "HD Graphics"},
	{0x4541, "HD Graphics"},
	{0x4E71, "HD Graphics"},
	{0x4E61, "HD Graphics"},
	{0x4E51, "HD Graphics"},

	/* TigerLake */
	{0x9A49, "HD Graphics"},
	{0x9A40, "HD Graphics"},
	{0x9A59, "HD Graphics"},
	{0x9A60, "HD Graphics"},
	{0x9A68, "HD Graphics"},
	{0x9A70, "HD Graphics"},
	{0x9A78, "HD Graphics"},

	/* When adding new identifiers, also update:
	 * 1. intel_identify()
	 * 2. man/intel.man
	 * 3. README
	 */

	{-1, NULL} /* Sentinel */
};

static const struct pci_id_match intel_device_match[] = {
#if UMS
	INTEL_VGA_DEVICE(PCI_CHIP_I810, &intel_i81x_info),
	INTEL_VGA_DEVICE(PCI_CHIP_I810_DC100, &intel_i81x_info),
	INTEL_VGA_DEVICE(PCI_CHIP_I810_E, &intel_i81x_info),
	INTEL_VGA_DEVICE(PCI_CHIP_I815, &intel_i81x_info),
#endif

#if KMS
	INTEL_I830_IDS(&intel_i830_info),
	INTEL_I845G_IDS(&intel_i845_info),
	INTEL_I85X_IDS(&intel_i855_info),
	INTEL_I865G_IDS(&intel_i865_info),

	INTEL_I915G_IDS(&intel_i915_info),
	INTEL_I915GM_IDS(&intel_i915_info),
	INTEL_I945G_IDS(&intel_i945_info),
	INTEL_I945GM_IDS(&intel_i945_info),

	INTEL_G33_IDS(&intel_g33_info),
	INTEL_PINEVIEW_G_IDS(&intel_g33_info),
	INTEL_PINEVIEW_M_IDS(&intel_g33_info),

	INTEL_I965G_IDS(&intel_i965_info),
	INTEL_I965GM_IDS(&intel_i965_info),

	INTEL_G45_IDS(&intel_g4x_info),
	INTEL_GM45_IDS(&intel_g4x_info),

	INTEL_IRONLAKE_D_IDS(&intel_ironlake_info),
	INTEL_IRONLAKE_M_IDS(&intel_ironlake_info),

	INTEL_SNB_D_IDS(&intel_sandybridge_info),
	INTEL_SNB_M_IDS(&intel_sandybridge_info),

	INTEL_IVB_D_IDS(&intel_ivybridge_info),
	INTEL_IVB_M_IDS(&intel_ivybridge_info),

	INTEL_HSW_IDS(&intel_haswell_info),
	INTEL_VLV_IDS(&intel_valleyview_info),

	INTEL_BDW_IDS(&intel_broadwell_info),
	INTEL_CHV_IDS(&intel_cherryview_info),

	INTEL_SKL_IDS(&intel_skylake_info),
	INTEL_BXT_IDS(&intel_broxton_info),
	INTEL_KBL_IDS(&intel_kabylake_info),
	INTEL_GLK_IDS(&intel_geminilake_info),
	INTEL_CFL_IDS(&intel_coffeelake_info),

	INTEL_CNL_IDS(&intel_cannonlake_info),

	INTEL_ICL_11_IDS(&intel_icelake_info),
	INTEL_EHL_IDS(&intel_elkhartlake_info),

	INTEL_TGL_12_IDS(&intel_tigerlake_info),

	INTEL_VGA_DEVICE(PCI_MATCH_ANY, &intel_generic_info),
#endif

	{},
};

void
intel_detect_chipset(ScrnInfoPtr scrn, struct intel_device *dev)
{
	int devid;
	const char *name = NULL;
	int i;

	if (dev == NULL) {
		EntityInfoPtr ent;
		struct pci_device *pci;

		ent = xf86GetEntityInfo(scrn->entityList[0]);
		if (ent->device->chipID >= 0) {
			xf86DrvMsg(scrn->scrnIndex, X_CONFIG,
				   "ChipID override: 0x%04X\n",
				   ent->device->chipID);
			devid = ent->device->chipID;
		} else {
			pci = xf86GetPciInfoForEntity(ent->index);
			if (pci)
				devid = pci->device_id;
			else
				devid = ~0;
		}
	} else
		devid = intel_get_device_id(dev);

	for (i = 0; intel_chipsets[i].name != NULL; i++) {
		if (devid == intel_chipsets[i].token) {
			name = intel_chipsets[i].name;
			break;
		}
	}
	if (name == NULL) {
		int gen = 0;

		for (i = 0; intel_device_match[i].device_id != 0; i++) {
			if (devid == intel_device_match[i].device_id) {
				const struct intel_device_info *info = (void *)intel_device_match[i].match_data;
				gen = info->gen >> 3;
				break;
			}
		}

		if (gen) {
			xf86DrvMsg(scrn->scrnIndex, X_PROBED,
				   "gen%d engineering sample\n", gen);
		} else {
			xf86DrvMsg(scrn->scrnIndex, X_WARNING,
				   "Unknown chipset\n");
		}

		name = "unknown";
	} else {
		xf86DrvMsg(scrn->scrnIndex, X_PROBED,
			   "Integrated Graphics Chipset: Intel(R) %s\n",
			   name);
	}

	scrn->chipset = (char *)name;
}

/*
 * intel_identify --
 *
 * Returns the string name for the driver based on the chipset.
 *
 */
static void intel_identify(int flags)
{
	const SymTabRec *chipset;
	const char *stack[64], **unique;
	int i, j, size, len;

	unique = stack;
	size = sizeof(stack)/sizeof(stack[0]);
	i = 0;

	xf86Msg(X_INFO, INTEL_NAME ": Driver for Intel(R) Integrated Graphics Chipsets:\n\t");
	len = 8;

	for (chipset = intel_chipsets; chipset->token; chipset++) {
		for (j = i; --j >= 0;)
			if (strcmp(unique[j], chipset->name) == 0)
				break;
		if (j < 0) {
			int name_len = strlen(chipset->name);
			if (i != 0) {
				xf86ErrorF(",");
				len++;
				if (len + 2 + name_len < 78) {
					xf86ErrorF(" ");
					len++;
				} else {
					xf86ErrorF("\n\t");
					len = 8;
				}
			}
			xf86ErrorF("%s", chipset->name);
			len += name_len;

			if (i == size) {
				const char **new_unique;

				if (unique == stack)
					new_unique = malloc(2*sizeof(*unique)*size);
				else
					new_unique = realloc(unique, 2*sizeof(*unique)*size);
				if (new_unique != NULL) {
					if (unique == stack)
						memcpy(new_unique, stack,
						       sizeof(stack));
					unique = new_unique;
					size *= 2;
				}
			}
			if (i < size)
				unique[i++] = chipset->name;
		}
	}
	xf86ErrorF("\n");
	if (unique != stack)
		free(unique);

	xf86Msg(X_INFO, INTEL_NAME ": Driver for Intel(R) HD Graphics\n");
	xf86Msg(X_INFO, INTEL_NAME ": Driver for Intel(R) Iris(TM) Graphics\n");
	xf86Msg(X_INFO, INTEL_NAME ": Driver for Intel(R) Iris(TM) Pro Graphics\n");
}

static Bool intel_driver_func(ScrnInfoPtr pScrn,
			      xorgDriverFuncOp op,
			      pointer ptr)
{
	xorgHWFlags *flag;

	switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
		flag = (CARD32*)ptr;
		(*flag) = 0;
#if UMS
		(*flag) = HW_IO | HW_MMIO;
#endif
#ifdef HW_SKIP_CONSOLE
		if (hosted())
			(*flag) = HW_SKIP_CONSOLE;
#endif

		return TRUE;

#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,15,99,902,0)
	case SUPPORTS_SERVER_FDS:
		return TRUE;
#endif

	default:
		/* Unknown or deprecated function */
		return FALSE;
	}
}

#if KMS
extern XF86ConfigPtr xf86configptr;

static XF86ConfDevicePtr
_xf86findDriver(const char *ident, XF86ConfDevicePtr p)
{
	while (p) {
		if (p->dev_driver && xf86NameCmp(ident, p->dev_driver) == 0)
			return p;

		p = p->list.next;
	}

	return NULL;
}

static enum accel_method { NOACCEL, SNA, UXA } get_accel_method(void)
{
	enum accel_method accel_method = DEFAULT_ACCEL_METHOD;
	XF86ConfDevicePtr dev;

	if (hosted())
		return SNA;

	if (xf86configptr == NULL) /* X -configure */
		return SNA;

	dev = _xf86findDriver("intel", xf86configptr->conf_device_lst);
	if (dev && dev->dev_option_lst) {
		const char *s;

		s = xf86FindOptionValue(dev->dev_option_lst, "AccelMethod");
		if (s ) {
			if (strcasecmp(s, "none") == 0)
				accel_method = NOACCEL;
			else if (strcasecmp(s, "sna") == 0)
				accel_method = SNA;
			else if (strcasecmp(s, "uxa") == 0)
				accel_method = UXA;
		}
	}

	return accel_method;
}
#endif

static Bool
intel_scrn_create(DriverPtr		driver,
		  int			entity_num,
		  intptr_t		match_data,
		  unsigned		flags)
{
	ScrnInfoPtr scrn;

	if (match_data == 0) {
		int devid = intel_entity_get_devid(entity_num), i;
		if (devid == 0)
			return FALSE;

		for (i = 0; intel_device_match[i].device_id != 0; i++) {
			if (devid == intel_device_match[i].device_id) {
				match_data = (intptr_t)&intel_device_match[i];
				break;
			}
		}

		if (match_data == 0)
			return FALSE;
	}

	scrn = xf86AllocateScreen(driver, flags);
	if (scrn == NULL)
		return FALSE;

	scrn->driverVersion = INTEL_VERSION;
	scrn->driverName = (char *)INTEL_DRIVER_NAME;
	scrn->name = (char *)INTEL_NAME;
	scrn->driverPrivate = (void *)(match_data | (flags & XF86_ALLOCATE_GPU_SCREEN) | 2);
	scrn->Probe = NULL;

	if (xf86IsEntitySharable(entity_num))
		xf86SetEntityShared(entity_num);
	xf86AddEntityToScreen(scrn, entity_num);

#if UMS
	if ((unsigned)((struct intel_device_info *)match_data)->gen < 020)
		return lg_i810_init(scrn);
#endif

#if KMS
	switch (get_accel_method()) {
#if USE_SNA
	case NOACCEL:
	case SNA:
		return sna_init_scrn(scrn, entity_num);
#endif
#if USE_UXA
#if !USE_SNA
	case NOACCEL:
#endif
	case UXA:
		return intel_init_scrn(scrn);
#endif

	default:
#if USE_SNA
		return sna_init_scrn(scrn, entity_num);
#elif USE_UXA
		return intel_init_scrn(scrn);
#else
		break;
#endif
	}
#endif

	return FALSE;
}

/*
 * intel_pci_probe --
 *
 * Look through the PCI bus to find cards that are intel boards.
 * Setup the dispatch table for the rest of the driver functions.
 *
 */
static Bool intel_pci_probe(DriverPtr		driver,
			    int			entity_num,
			    struct pci_device	*pci,
			    intptr_t		match_data)
{
	Bool ret;

	if (intel_open_device(entity_num, pci, NULL) == -1) {
#if UMS
		switch (pci->device_id) {
		case PCI_CHIP_I810:
		case PCI_CHIP_I810_DC100:
		case PCI_CHIP_I810_E:
		case PCI_CHIP_I815:
			if (!hosted())
				break;
			/* fall through */
		default:
			return FALSE;
		}
#else
		return FALSE;
#endif
	}

	ret = intel_scrn_create(driver, entity_num, match_data, 0);
	if (!ret)
		intel_close_device(entity_num);

	return ret;
}

#ifdef XSERVER_PLATFORM_BUS
static Bool
intel_platform_probe(DriverPtr driver,
		     int entity_num, int flags,
		     struct xf86_platform_device *dev,
		     intptr_t match_data)
{
	unsigned scrn_flags = 0;

	if (intel_open_device(entity_num, dev->pdev, dev) == -1)
		return FALSE;

	/* Allow ourselves to act as a slaved output if not primary */
	if (flags & PLATFORM_PROBE_GPU_SCREEN) {
		flags &= ~PLATFORM_PROBE_GPU_SCREEN;
		scrn_flags |= XF86_ALLOCATE_GPU_SCREEN;
	}

	/* if we get any flags we don't understand fail to probe for now */
	if (flags)
		goto err;

	if (!intel_scrn_create(driver, entity_num, match_data, scrn_flags))
		goto err;

	return TRUE;

err:
	intel_close_device(entity_num);
	return FALSE;
}
#endif

#ifdef XFree86LOADER

static MODULESETUPPROTO(intel_setup);

static XF86ModuleVersionInfo intel_version = {
	"intel",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	INTEL_VERSION_MAJOR, INTEL_VERSION_MINOR, INTEL_VERSION_PATCH,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	MOD_CLASS_VIDEODRV,
	{0, 0, 0, 0}
};

static const OptionInfoRec *
intel_available_options(int chipid, int busid)
{
	switch (chipid) {
#if UMS
	case PCI_CHIP_I810:
	case PCI_CHIP_I810_DC100:
	case PCI_CHIP_I810_E:
	case PCI_CHIP_I815:
		return lg_i810_available_options(chipid, busid);
#endif

	default:
		return intel_options;
	}
}

static DriverRec intel = {
	INTEL_VERSION,
	(char *)INTEL_DRIVER_NAME,
	intel_identify,
	NULL,
	intel_available_options,
	NULL,
	0,
	intel_driver_func,
	intel_device_match,
	intel_pci_probe,
#ifdef XSERVER_PLATFORM_BUS
	intel_platform_probe
#endif
};

static pointer intel_setup(pointer module,
			   pointer opts,
			   int *errmaj,
			   int *errmin)
{
	static Bool setupDone = 0;

	/* This module should be loaded only once, but check to be sure.
	*/
	if (!setupDone) {
		setupDone = 1;
		xf86AddDriver(&intel, module, HaveDriverFuncs);

		/*
		 * The return value must be non-NULL on success even though there
		 * is no TearDownProc.
		 */
		return (pointer) 1;
	} else {
		if (errmaj)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
}

_X_EXPORT XF86ModuleData intelModuleData = { &intel_version, intel_setup, NULL };
#endif
