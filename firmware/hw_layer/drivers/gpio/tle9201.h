/*
 * tle9201.h
 *
 * TLE9201 H-Bridge with SPI
 *
 * @date Nov 1, 2024
 *
 * @author andreika, (c) 2024
 * @author Andrey Belomutskiy, (c) 2012-2024
 */

#pragma once

#include "efifeatures.h"
#include <hal.h>

// 10 Hz
#define TLE9201_POLL_INTERVAL_MS	100

// Diag reports print only after the byte has been STABLE for this many
// consecutive polls. The open-load flag of a chip with no blade load
// (bench) oscillates on every ETB PWM activation, so print-on-change
// flooded the log; N=4 (400 ms) suppresses the oscillation while a
// latched fault still settles and reports.
#define TLE9201_DIAG_STABLE_POLLS	4

// used by tle9201_config.spi_config.cr1 & .cr2
#define TLE9201_CONFIG_CR1 (\
	SPI_CR1_8BIT_MODE | \
	SPI_CR1_SSM | \
	SPI_CR1_SSI | \
	((0b110 << SPI_CR1_BR_Pos) & SPI_CR1_BR) | \
	SPI_CR1_MSTR | \
	SPI_CR1_CPHA)
#define TLE9201_CONFIG_CR2 (SPI_CR2_8BIT_MODE)

struct tle9201_config {
#if HAL_USE_SPI
	SPIDriver		*spi_bus;
	SPIConfig	spi_config;
#endif
};

int tle9201_add(unsigned int index, const struct tle9201_config *cfg);
