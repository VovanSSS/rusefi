/**
 * @file	loggingcentral.h
 *
 * @date Mar 8, 2015
 * @author Andrey Belomutskiy, (c) 2012-2020
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include "generated_lookup_meta.h"

class Logging;

void startLoggingProcessor();

const char* swapOutputBuffers(size_t *actualOutputBufferSize);

namespace priv
{
	// internal implementation, use efiPrintf below
	void efiPrintfInternal(const char *fmt, ...)
		#if EFI_PROD_CODE
			__attribute__ ((format (printf, 1, 2)))
		#endif
			;
}

// "normal" logging messages need a header and footer, so put them in
// the format string at compile time
#define efiPrintfProto(proto, fmt, ...) priv::efiPrintfInternal(proto LOG_DELIMITER fmt LOG_DELIMITER, ##__VA_ARGS__)
#define efiPrintf(fmt, ...) efiPrintfProto(PROTOCOL_MSG, fmt, ##__VA_ARGS__)

/**
 * This is the legacy function to copy the contents of a local Logging object in to the output buffer
 */
void scheduleLogging(Logging *logging);

// Stores the result of one call to efiPrintfInternal in the queue to be copied out to the output buffer
struct LogLineBuffer {
	char buffer[256];
};

template <size_t TBufferSize>
class LogBuffer {
public:
	void writeLine(LogLineBuffer* line);
	void writeLogger(Logging* logging);

	size_t length() const;
	void reset();
	const char* get() const;

	// Diagnostic accessors: expose the internals so swapOutputBuffers can
	// report WHICH field was clobbered and to what value when the buffer
	// accounting check trips (memory-corruption triage).
	uintptr_t diagWritePtr() const { return reinterpret_cast<uintptr_t>(m_writePtr); }
	uintptr_t diagBase() const { return reinterpret_cast<uintptr_t>(m_buffer); }

#if !EFI_UNIT_TEST
private:
#endif
	void writeInternal(const char* buffer);

	char m_buffer[TBufferSize];
	char* m_writePtr = m_buffer;
};
