#include "pch.h"
#include "flash_int.h"

extern "C" {
	#include "boot.h"
	#include "flash.h"
}

#if defined(STM32H7XX) || defined(AT32F435xx)
//Since a 10-bit ECC code is associated to each 256-bit data Flash word,
//only write operations by 256 bits are executed in the non-volatile memory.
//The AT32F435 flash likewise rejects a second program to a word that is not
//fully erased (EPPERR), so each 32-bit word may be programmed exactly once:
//buffer the incoming bytes and flush whole words on line boundaries.
#ifdef STM32H7XX
#define FLASH_ECC_LINE_SIZE 	(256 / 8)
#else
#define FLASH_ECC_LINE_SIZE 	(32 / 8)
#endif

static blt_int8u flashBuffer[FLASH_ECC_LINE_SIZE];
static blt_addr flashAddr = 0x0;

#define FLASH_WRITE_STEP		FLASH_ECC_LINE_SIZE
#define FLASH_WRITE_ADDR_MASK	(~(FLASH_WRITE_STEP - 1))

static void FlashBufferReset()
{
	memset(flashBuffer, 0xff, sizeof(flashBuffer));
	flashAddr = 0;
}

static blt_bool FlashBufferFlush()
{
	if (flashAddr == 0x0) {
		return BLT_TRUE;
	}

	int result = intFlashWrite(flashAddr, (const char*)flashBuffer, sizeof(flashBuffer));

	FlashBufferReset();

	return (result == FLASH_RETURN_SUCCESS) ? BLT_TRUE : BLT_FALSE;
}

static blt_bool FlashBufferedWrite(blt_addr addr, blt_int32u len, blt_int8u *data)
{
	while (len) {
		if ((addr & FLASH_WRITE_ADDR_MASK) != flashAddr) {
			// crossing flash line boundary - program the buffered line
			FlashBufferFlush();

			flashAddr = addr & FLASH_WRITE_ADDR_MASK;
		}

		off_t off = addr - flashAddr;
		size_t chunk = minI(sizeof(flashBuffer) - off, len);
		memcpy(flashBuffer + off, data, chunk);

		addr += chunk;
		data += chunk;
		len -= chunk;
	}

	return BLT_TRUE;
}

#endif

#ifdef M74_9_OEM_LOADER
// Three-stage boot chain behind the OEM resident loader. OpenBLT owns the
// fixed application contract (vectors 0x08001000, code 0x08080000-0x0808FFFF,
// activation descriptor 0x0805FFE0) and may only write the relocated main
// rusEFI application. The CRC domain is the full application image (both
// software segments), CRC-32/MPEG-2, little-endian word at 0x080FFFFC.
static const blt_addr kMainSeg1Start = 0x08010000;
static const blt_addr kMainSeg1End   = 0x0805F000;  // descriptor sector boundary
static const blt_addr kMainSeg2Start = 0x08090000;
static const blt_addr kMainSeg2Crc   = 0x080FFFFC;  // CRC word
static const blt_addr kMainSeg2End   = 0x08100000;  // sector-aligned end

static bool inWriteRange(blt_addr addr, blt_int32u len) {
	blt_addr end = addr + len;
	return (addr >= kMainSeg1Start && end <= kMainSeg1End) ||
	       (addr >= kMainSeg2Start && end <= kMainSeg2Crc);
}

static bool inEraseRange(blt_addr addr, blt_int32u len) {
	blt_addr end = addr + len;
	return (addr >= kMainSeg1Start && end <= kMainSeg1End) ||
	       (addr >= kMainSeg2Start && end <= kMainSeg2End);
}

static uint32_t crc32Mpeg2Byte(uint32_t crc, uint8_t byte) {
	crc ^= uint32_t(byte) << 24;
	for (int bit = 0; bit < 8; bit++) {
		crc = (crc << 1) ^ ((crc & 0x80000000U) ? 0x04C11DB7U : 0U);
	}
	return crc;
}

static uint32_t crc32Mpeg2Word(uint32_t crc, uint32_t word) {
	crc = crc32Mpeg2Byte(crc, uint8_t(word));
	crc = crc32Mpeg2Byte(crc, uint8_t(word >> 8));
	crc = crc32Mpeg2Byte(crc, uint8_t(word >> 16));
	crc = crc32Mpeg2Byte(crc, uint8_t(word >> 24));
	return crc;
}

// Application CRC over both software segments (excluding the CRC word itself):
//   0x08001000-0x0805FFFF and 0x08080000-0x080FFFFB.
static uint32_t computeAppCrc() {
	uint32_t crc = 0xFFFFFFFF;
	for (uint32_t a = 0x08001000; a < 0x08060000; a += 4) {
		crc = crc32Mpeg2Word(crc, *reinterpret_cast<const volatile uint32_t*>(a));
	}
	for (uint32_t a = 0x08080000; a < 0x080FFFFC; a += 4) {
		crc = crc32Mpeg2Word(crc, *reinterpret_cast<const volatile uint32_t*>(a));
	}
	return crc;
}
#endif // M74_9_OEM_LOADER

void FlashInit() {
	// Flash already init by ChibiOS
#if defined(STM32H7XX) || defined(AT32F435xx)
	FlashBufferReset();
#endif
}

blt_addr FlashGetUserProgOffset() {
#ifdef M74_9_OEM_LOADER
	return 0x10000;  // main app vector table at 0x08010000
#elif defined(STM32H7XX)
	return 128 * 1024;
#else // not STM32H7
	return 32 * 1024;
#endif
}

blt_addr FlashGetUserProgBaseAddress() {
	return FLASH_BASE + FlashGetUserProgOffset();
}

blt_bool FlashWrite(blt_addr addr, blt_int32u len, blt_int8u *data) {
#ifdef M74_9_OEM_LOADER
	// don't allow overwriting OpenBLT, the descriptor, calibration or NVM
	if (!inWriteRange(addr, len)) {
		return BLT_FALSE;
	}
#else
	// don't allow overwriting the bootloader
	if (addr < FlashGetUserProgBaseAddress()) {
		return BLT_FALSE;
	}
#endif

#if defined(STM32H7XX) || defined(AT32F435xx)
	return FlashBufferedWrite(addr, len, data);
#else // not STM32H7 / AT32F435
	return (FLASH_RETURN_SUCCESS == intFlashWrite(addr, (const char*)data, len)) ? BLT_TRUE : BLT_FALSE;
#endif
}

blt_bool FlashErase(blt_addr addr, blt_int32u len) {
#ifdef M74_9_OEM_LOADER
	if (!inEraseRange(addr, len)) {
		return BLT_FALSE;
	}
#else
	// don't allow erasing the bootloader
	if (addr < FlashGetUserProgBaseAddress()) {
		return BLT_FALSE;
	}
#endif

	if (!intFlashIsErased(addr, len)) {
		return (FLASH_RETURN_SUCCESS == intFlashErase(addr, len)) ? BLT_TRUE : BLT_FALSE;
	}

	return BLT_TRUE;
}

blt_bool FlashDone() {
#if defined(STM32H7XX) || defined(AT32F435xx)
	FlashBufferFlush();
#endif
	return BLT_TRUE;
}

blt_bool FlashWriteChecksum() {
#ifdef M74_9_OEM_LOADER
#if defined(STM32H7XX) || defined(AT32F435xx)
	// CRITICAL ORDER (2026-10-02 brick root cause): NvmDone() calls
	// FlashWriteChecksum() BEFORE FlashDone(), and the 4-byte write buffer
	// still holds the last line of the program data at that point. Flush it
	// FIRST or computeAppCrc() reads a 0xFFFFFFFF tail and stores a CRC that
	// the finalized image no longer matches - the next boot's checkImages
	// rejects the app and the ECU stays stuck in XCP mode (console dead,
	// OpenBLT probe alive). Measured on the bench: stored 0x54F37818 =
	// CRC with the last word still erased vs the correct 0xDA36F642.
	if (FlashBufferFlush() == BLT_FALSE) {
		return BLT_FALSE;
	}
#endif
	// The host erases only up to the end of the program data (the app-only
	// .srec does not include the CRC word), so the CRC sector 0x080FF000..
	// 0x080FFFFF may still hold the previous CRC and is not erased. Erase it
	// first - it holds only 0xFF padding plus the old CRC - then program.
	uint32_t crc = computeAppCrc();
	if (!intFlashIsErased(0x080FFFFC, 4)) {
		if (FLASH_RETURN_SUCCESS != intFlashErase(0x080FF000, 0x1000)) {
			return BLT_FALSE;
		}
	}
	if (FLASH_RETURN_SUCCESS != intFlashWrite(0x080FFFFC, (const char*)&crc, 4)) {
		return BLT_FALSE;
	}
	// Read back the stored word: a silent write failure here is invisible to
	// the per-segment BUILD_CHECKSUM verify and bricks the next boot.
	return (*reinterpret_cast<const volatile uint32_t*>(0x080FFFFC) == crc) ? BLT_TRUE : BLT_FALSE;
#else
	return BLT_TRUE;
#endif
}

blt_bool FlashVerifyChecksum() {
#ifdef M74_9_OEM_LOADER
	uint32_t calculated = computeAppCrc();
	uint32_t stored = *reinterpret_cast<const volatile uint32_t*>(0x080FFFFC);
	return calculated == stored ? BLT_TRUE : BLT_FALSE;
#else
	// Naive check: if the first block is blank, there's no code there
	if (intFlashIsErased(FlashGetUserProgBaseAddress(), 4)) {
		return BLT_FALSE;
	}

	static const size_t checksumOffset = 0x1C;

	// Now do the actual CRC check to ensure we didn't get stuck with a half-written firmware image
	uint8_t* start = reinterpret_cast<uint8_t*>(FlashGetUserProgBaseAddress());

	size_t imageSize = *reinterpret_cast<size_t*>(start + checksumOffset + 4);

	if (imageSize > (flashSizeKb() * 1024 - FlashGetUserProgOffset())) {
		// impossibly large size, invalid
		return BLT_FALSE;
	}

	// part before checksum+size
	uint32_t calcChecksum = crc32(start, checksumOffset);
	// part after checksum+size
	calcChecksum = crc32inc(start + checksumOffset + 4, calcChecksum, imageSize - (checksumOffset + 4));

	uint32_t storedChecksum = *reinterpret_cast<uint32_t*>(start + checksumOffset);

	return calcChecksum == storedChecksum ? BLT_TRUE : BLT_FALSE;
#endif
}

blt_bool isFlashDualBank(void) {
#ifdef STM32F7XX
	// cleared bit indicates dual bank
	return (FLASH->OPTCR & FLASH_OPTCR_nDBANK) == 0 ? BLT_TRUE : BLT_FALSE;
#else
	return BLT_TRUE;
#endif
}
