/*
 * Copyright (C) 2014 Oleksij Rempel <linux@rempel-privat.de>
 *  Co-author: Du Huanpeng <u74147@gmail.com>
 * map_desc based on:
 *  linux/arch/arm/mach-asm9260/core.c
 *  Copyright (C) 2011-2014 Alphascale
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/of_platform.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

static struct map_desc asm9260_io_desc[] __initdata = {
	{	/* IO space */
		.virtual	= (unsigned long)0xf0000000,
		.pfn		= __phys_to_pfn(0x80000000),
		.length		= 0x00800000,
		.type		= MT_DEVICE
	},
	{	/* LCD IO space	*/
		.virtual	= (unsigned long)0xf0a00000,
		.pfn		= __phys_to_pfn(0x80800000),
		.length		= 0x00009000,
		.type		= MT_DEVICE
	},
	{	/* GPIO IO space */
		.virtual	= (unsigned long)0xf0800000,
		.pfn		= __phys_to_pfn(0x50000000),
		.length		= 0x00100000,
		.type		= MT_DEVICE
	},
};

static void __init asm9260_map_io(void)
{
	iotable_init(asm9260_io_desc, ARRAY_SIZE(asm9260_io_desc));
}

static const char * const asm9260_dt_board_compat[] __initconst = {
	"alphascale,asm9260",
	NULL
};

DT_MACHINE_START(ASM9260, "Alphascale ASM9260 (Device Tree Support)")
	.map_io		= asm9260_map_io,
	.dt_compat	= asm9260_dt_board_compat,
MACHINE_END
