ENTRY( _start )

/* 32K, raised from 16K when Ethernet arrived.
 *
 * lwip's receive path is a deep C call chain -- ethernet_input, ip4_input,
 * tcp_input, the socket callback -- and it runs on top of whatever depth the
 * Python code that pumped it had already reached, which for a blocking
 * socket.recv() is considerable. At 16K a sustained TCP download overran the
 * stack into the heap below it and the board reset without even managing a
 * fault message, because the fault handler had no stack left either.
 *
 * ch32.stack_usage() reports the high-water mark; see modch32.c. */
__stack_size = 32768;

PROVIDE( _stack_size = __stack_size );

MEMORY
{
	/* ORIGIN must match CH32_V5F_FLASH_ORIGIN in ../../coreboot.mk, and the end
	   of this region must match CH32_FLASH_FS_BASE in ../../flash.h: the FAT
	   volume starts exactly where this stops. It used to run to the end of the
	   chip, which let the image grow silently into the volume -- see the
	   comment in flash.h. Overflowing this region is now a link error. */
	FLASH (rx)     : ORIGIN = 0x00010000, LENGTH = 512K
	/* Shared region: .text/.rodata, copied from flash by startup. Runs at HCLK
	   (100 MHz) rather than the V5F core clock; task 6 evaluates alternatives. */
	ITCM (xrw)     : ORIGIN = 0x200A0000, LENGTH = 128K
	/* 368K, and the size is set by the *generic* toolchain, not the WCH one:
	   stock GCC without the xw extension emits about 15K more for the same
	   sources, so CI overflows this region while a local build still links.
	   Check with CH32_TOOLCHAIN=generic before trusting a size here.

	   History, because it has only gone one way: 240K until mbedtls (+70K of
	   TLS), 320K until the RV32 emitter (+25K of backend), then 336K, and now
	   368K for machine.CAN and machine.Timer. Every increase came out of
	   HEAP2. The next lever is not this boundary -- it is moving cold code
	   (X.509 parsing, oofatfs, USB descriptors) back to execute from flash,
	   which is a quarter of the speed but is code that runs once. */
	RAM_CODE (xrw) : ORIGIN = 0x20100000, LENGTH = 368K
	/* USBFS DMA cannot reach DTCM, so endpoint buffers live here, in
	   the shared region on the system bus. */
	USB_RAM (xrw)  : ORIGIN = 0x2015C000, LENGTH = 16K
	/* Same reason as USB_RAM: the Ethernet DMA is on the system bus and cannot
	   reach DTCM either. Descriptors and MAC frame buffers live here. */
	ETH_RAM (xrw)  : ORIGIN = 0x20160000, LENGTH = 32K
	/* The SDMMC bounce buffers, two runs of eight blocks. Its own region
	   rather than a corner of RAM_CODE, which has no room for it: the generic
	   toolchain emits ~15K more and overflowed by 128 bytes with these here.
	   The address itself does not matter to the DMA -- it reads and writes
	   RAM_CODE, ETH_RAM and this equally -- so this is only about space. */
	SD_RAM (rw)    : ORIGIN = 0x20168000, LENGTH = 8K
	/* The rest of the shared area, out to its end at 0x20180000. Nothing else
	   had claimed it, so it goes to the GC as a second heap area -- see
	   MICROPY_GC_SPLIT_HEAP and the gc_add() call in main.c. It takes the heap
	   from the 162K that fits in DTCM to about 250K. */
	HEAP2 (rw)     : ORIGIN = 0x2016A000, LENGTH = 88K
	/* DTCM: zero-wait at the V5F core clock. Holds .data/.bss/stack/heap. */
	RAM_LOAD (xrw) : ORIGIN = 0x200C0000, LENGTH = 256
	RAM (xrw)      : ORIGIN = (0x200C0000+256), LENGTH = (256K-256)
}

SECTIONS
{
	.init :
	{
		_sinit = .;
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.init)))
		. = ALIGN(4);
		_einit = .;
	} >FLASH AT>FLASH

    .loadcodelalign : 
    {       
        . = ALIGN(4);
        PROVIDE(_loadcode_lma = .); 
    } >FLASH AT>FLASH 

	.loadcode :
	{
		. = ALIGN(4);
        PROVIDE(_loadcode_vma_start = .);
		*(.load);
		. = ALIGN(4); 
		PROVIDE(_loadcode_vma_end = .);
	}>RAM_LOAD AT>FLASH

    .itcm_text :
    {
        . = ALIGN(4);
        PROVIDE(_itcm_vma_start = .);
        /* The whole interpreter core. ITCM is 128K; anything that does not
           fit here falls through to .highcode in the shared region.

           Except the native code generator. asmrv32.o, emitnrv32.o and
           emitinlinerv32.o are the RV32 backend for @micropython.native,
           @micropython.viper and @micropython.asm_rv32, and they run while
           *compiling* a decorated function, not while running one -- the
           output runs from the heap either way. They are also 25K between
           them, which is more than ITCM had left, so admitting them would push
           the bytecode VM out into the shared region and slow down every
           program to speed up compilation of the few that use them. */
        */py/*.o(
            EXCLUDE_FILE(*/asmrv32.o */emitnrv32.o */emitinlinerv32.o) .text
            EXCLUDE_FILE(*/asmrv32.o */emitnrv32.o */emitinlinerv32.o) .text.*)
        */shared/runtime/*.o(.text .text.*)
        /* framebuf's fill and blit are instruction-fetch bound out in the
           shared region: they measure identically whether their pixels live
           in DTCM or shared SRAM, because what limits them is fetching the
           loop at HCLK while the core runs at 4x that. */
        */extmod/modframebuf.o(.text .text.*)
        /* ...and the fast paths that replace those loops, for the same
           reason. Putting modframebuf here and leaving these behind would
           just move the bottleneck. */
        *framebuf_accel.o(.text .text.*)
        *gpha.o(.text .text.*)
        . = ALIGN(4);
        PROVIDE(_itcm_vma_end = .);
    } >ITCM AT>FLASH
    PROVIDE(_itcm_lma = LOADADDR(.itcm_text));

    .highcodelalign :
    {       
        . = ALIGN(4);
        PROVIDE(_highcode_lma = .); 
    } >FLASH AT>FLASH 
    
    .highcode : 
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        *(.vector);
		KEEP(*(SORT_NONE(.vector_handler)))
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata*)
        *(.sdata2.*)
		*(.glue_7)
		*(.glue_7t)
		*(.gnu.linkonce.t.*)
		. = ALIGN(4); 
        PROVIDE(_highcode_vma_end = .);
    } >RAM_CODE AT>FLASH

	.text :
	{
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.handle_reset)))
		. = ALIGN(4);
	} >FLASH AT>FLASH 

    .highcodelalign1 : 
    {       
        . = ALIGN(4);
        PROVIDE(_highcode_lma1 = .); 
    } >FLASH AT>FLASH 

	.fini :
	{
	    . = ALIGN(4);
        PROVIDE(_highcode_vma_start1 = .);
		KEEP(*(SORT_NONE(.fini)))
		. = ALIGN(4);
	} >RAM_CODE AT>FLASH

	PROVIDE( _etext = . );
	PROVIDE( _eitcm = . );	

	.preinit_array  :
	{
	  PROVIDE_HIDDEN (__preinit_array_start = .);
	  KEEP (*(.preinit_array))
	  PROVIDE_HIDDEN (__preinit_array_end = .);
	} >RAM_CODE AT>FLASH 
	
	.init_array     :
	{
	  PROVIDE_HIDDEN (__init_array_start = .);
	  KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
	  KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
	  PROVIDE_HIDDEN (__init_array_end = .);
	} >RAM_CODE AT>FLASH 
	
	.fini_array     :
	{
	  PROVIDE_HIDDEN (__fini_array_start = .);
	  KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
	  KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
	  PROVIDE_HIDDEN (__fini_array_end = .);
	} >RAM_CODE AT>FLASH 
	
	.ctors          :
	{
	  /* gcc uses crtbegin.o to find the start of
	     the constructors, so we make sure it is
	     first.  Because this is a wildcard, it
	     doesn't matter if the user does not
	     actually link against crtbegin.o; the
	     linker won't look for a file to match a
	     wildcard.  The wildcard also means that it
	     doesn't matter which directory crtbegin.o
	     is in.  */
	  KEEP (*crtbegin.o(.ctors))
	  KEEP (*crtbegin?.o(.ctors))
	  /* We don't want to include the .ctor section from
	     the crtend.o file until after the sorted ctors.
	     The .ctor section from the crtend file contains the
	     end of ctors marker and it must be last */
	  KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
	  KEEP (*(SORT(.ctors.*)))
	  KEEP (*(.ctors))
	} >RAM_CODE AT>FLASH 
	
	.dtors          :
	{
	  KEEP (*crtbegin.o(.dtors))
	  KEEP (*crtbegin?.o(.dtors))
	  KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
	  KEEP (*(SORT(.dtors.*)))
	  KEEP (*(.dtors))
	  . = ALIGN(4); 
      PROVIDE(_highcode_vma_end1 = .);
	} >RAM_CODE AT>FLASH 

	/* TinyUSB's state lives here so the USB DMA can reach it. This is a
	 * separate section from .bss, so the startup code's _sbss.._ebss clear does
	 * NOT cover it -- ch32_usbd_init() zeroes it explicitly using these
	 * symbols. Without that, every TinyUSB buffer and state variable starts as
	 * whatever the RAM happened to hold. */
	.usbram (NOLOAD) :
	{
		. = ALIGN(8);
		PROVIDE( _susbram = . );
		*/tinyusb/*(.bss .bss.* COMMON)
		. = ALIGN(8);
		PROVIDE( _eusbram = . );
	} >USB_RAM

	/* Ethernet descriptors and frame buffers, for the same reason .usbram
	 * exists. Also outside .bss, so eth_init() zeroes it explicitly using
	 * these symbols -- the startup code's _sbss.._ebss clear does not reach
	 * here. Descriptors need 32-byte alignment; the DMA reads them as bursts. */
	.ethram (NOLOAD) :
	{
		. = ALIGN(32);
		PROVIDE( _sethram = . );
		*(.ethram .ethram.*)
		. = ALIGN(32);
		PROVIDE( _eethram = . );
	} >ETH_RAM

	/* The SDMMC bounce buffers. Outside .bss for the same reason .usbram and
	 * .ethram are: that controller's DMA is on the system bus and cannot see
	 * DTCM, where .bss lives. Nothing zeroes it and nothing needs to -- every
	 * use fills it first. */
	.sdmmcram (NOLOAD) :
	{
		. = ALIGN(16);
		*(.sdmmcram .sdmmcram.*)
		. = ALIGN(16);
	} >SD_RAM

	/* Second GC heap area, handed to gc_add() after every gc_init(). Nothing
	 * is copied here at boot and nothing clears it: the GC writes its own area
	 * header and allocation tables into the front of it each time.
	 *
	 * Spelled as a section covering the whole region rather than as two bare
	 * symbol assignments so that anything later placed in HEAP2 overflows the
	 * region and fails the link, instead of quietly sharing memory with the
	 * heap and being overwritten on the first allocation. */
	.heap2 (NOLOAD) :
	{
		. = ALIGN(8);
		PROVIDE( _sheap2 = . );
		. = ORIGIN(HEAP2) + LENGTH(HEAP2);
		PROVIDE( _eheap2 = . );
	} >HEAP2

	.dalign :
	{
		. = ALIGN(4);
		PROVIDE(_data_vma = .);
	} >RAM AT>FLASH	

	.dlalign :
	{
		. = ALIGN(4); 
		PROVIDE(_data_lma = .);
	} >FLASH AT>FLASH

	.data :
	{
		. = ALIGN(4);
    	*(.gnu.linkonce.r.*)
    	*(.data .data.*)
    	*(.gnu.linkonce.d.*)
		. = ALIGN(8);
    	PROVIDE( __global_pointer$ = . + 0x800 );
    	*(.sdata .sdata.*)
		*(.sdata2.*)
    	*(.gnu.linkonce.s.*)
    	. = ALIGN(8);
    	*(.srodata.cst16)
    	*(.srodata.cst8)
    	*(.srodata.cst4)
    	*(.srodata.cst2)
    	*(.srodata .srodata.*)
    	. = ALIGN(4);
		PROVIDE( _edata = .);
	} >RAM AT>FLASH

	.bss :
	{
		. = ALIGN(4);
		PROVIDE( _sbss = .);
  	    *(.sbss*)
        *(.gnu.linkonce.sb.*)
		*(.bss*)
     	*(.gnu.linkonce.b.*)		
		*(COMMON*)
		. = ALIGN(4);
		PROVIDE( _ebss = .);
	} >RAM AT>FLASH

	/* machine.mem_backup(). Its own NOLOAD section, deliberately outside .data
	 * and .bss, because those are the two things startup copies and clears --
	 * being in neither is the entire mechanism by which this survives a reset.
	 * It is before _heap_start, so the GC never sees it either. */
	.mem_backup (NOLOAD) :
	{
		. = ALIGN(8);
		KEEP(*(.mem_backup))
		. = ALIGN(8);
	} >RAM

	PROVIDE( _end = _ebss);
	PROVIDE( end = . );
	PROVIDE( _heap_start = . );  
	PROVIDE( __heap_start = . );  

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        PROVIDE( _heap_end = . );    
        PROVIDE( __heap_end = . );  
        . = ALIGN(4);
        PROVIDE(_susrstack = . );
        . = . + __stack_size;
        PROVIDE( _eusrstack = .);
    } >RAM 

}



