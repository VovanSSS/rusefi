# this header is relevant both for modern driver to access internal flash, and for external flash persistence
# see also 'HAL_USE_EFL' in case of internal flash

# use higher level API instead
DDEFS += -DEFI_STORAGE_MFS=TRUE
# Are we going to store something else but settings?
DDEFS += -DMFS_CFG_MAX_RECORDS=8 -DMFS_CFG_TRANSACTION_MAX=0

# The full settings record (persistent_config_container_s) is stored in MFS on
# boards that set EFI_STORAGE_INT_FLASH=FALSE (m74_9, at_start_f435). That
# container is larger than ChibiOS' default MFS_CFG_MAX_RECORD_SIZE of 16 KB -
# sizeof(persistent_config_s) alone is 19432 bytes on m74_9, so the container
# is ~19.4 KB. The 16 KB limit makes mfsWriteRecord()'s parameter check
# (n <= MFS_CFG_MAX_RECORD_SIZE) trip an assert and the ECU watchdog-resets on
# every settings burn. Bump the limit with headroom for config growth.
DDEFS += -DMFS_CFG_MAX_RECORD_SIZE=32768
