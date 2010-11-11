/**
 * Definitions for Walb log device.
 *
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#ifndef WALB_LOG_DEVICE_H
#define WALB_LOG_DEVICE_H

/* #include <linux/list.h> */
#include "walb_log_record.h"

/*
 * FORMAT: Log pack.
 *
 * ASSUMPTION1: type must be the fixed-size data like C structure.
 * ASSUMPTION2: sizeof(type) is the data size of the type.
 * ASSUMPTION3: sizeof(name) is the total data size of items defined as the name.
 * ASSUMPTION4: size must be finite value. [] as infinite value is allowd only in the last definition.
 *
 * DEFINITION1: DATA type name[size] (description)
 * DEFINITION2: a sequence of DATA definition.
 * DEFINITION3: name { DATA DEFINITION }[size] (name or size=1 can be omitted.)
 * DEFINITION4: for i in [items or range]; { DATA DEFINITION using i }
 * DEFINITION5: if (predicate) { DATA DEFINITION }
 *
 * log_pack {
 *   log_header {
 *     DATA walb_log_record header[N_LOG_RECORD_IN_SECTOR]
 *     DATA u8 padding[SECTOR_SIZE - sizeof(header)]
 *   }
 *   for i in [0...N_LOG_RECORD_IN_SECTOR] {
 *     if (header[i].is_exist) {
 *       DATA u8 io_data[header[i].size * SECTOR_SIZE]
 *     }
 *   }
 * }
 *
 * PROPERTY1: sizeof(log_pack) % SECTOR_SIZE is 0.
 * PROPERTY2: sizeof(log_header) is SECTOR_SIZE.
 * PROPERTY3: offset of i'th io_data is walb_lsid_to_offset(header[i].lsid).
 * PROPERTY4: offset of log_pack is
 *            walb_lsid_to_offset(header[i].lsid - header[i].lsid_local) for all i.
 * PROPERTY5: sizeof(log_pack)
 *            log_pack_size = 1 + sum(header[i].size for all i).
 * PROPERTY6: next lsid.
 *            next_lsid = lsid + log_pack_size + 1.
 */

/*
 * FORMAT: Meta data of the log device.
 * 
 * log_device_meta_data {
 *   DATA u8 reserved[PAGE_SIZE]
 *   DATA walb_super_sector super0
 *   DATA u8 padding[PAGE_SIZE - SECTOR_SIZE]
 *   snapshot_meta_data {
 *     DATA walb_snapshot_sector snapshot_sector[super0.snapshot_metadata_size]
 *   }
 *   DATA walb_super_sector super1
 *   DATA u8 padding[PAGE_SIZE - SECTOR_SIZE]
 * }
 *
 *
 * PROPERTY1: Offset of super0
 *            n_sector_in_page = PAGE_SIZE / SECTOR_SIZE.
 *            offset_super0 = n_sector_in_page.
 * PROPERTY2: Offset of super1
 *            offset_super1 = offset_super0 + n_sector_in_page + super0.snapshot_metadata_size.
 * PROPERTY3: sizeof(log_device_meta_data)
 *            offset_super1 + n_sector_in_page.
 */

/*
 * FORMAT: Log device.
 *
 * log_device {
 *   log_device_meta_data
 *   ring_buffer {
 *     DATA u8[super0.ring_buffer_size * SECTOR_SIZE]
 *   }
 * }
 *
 * PROPERTY1: Offset of ring_buffer
 *            offset_ring_buffer = sizeof(log_device_meta_data).
 * PROPERTY2: Offset of of a given lsid is walb_lsid_to_offset(lsid).
 *
 *   u64 walb_lsid_to_offset(u64 lsid) {
 *       return offset_ring_buffer + (lsid % super0.ring_buffer_size);
 *   }
 *
 * PROPERTY3: Offset of log_pack of a given lsid and lsid_local.
 *            offset_log_pack = walb_lsid_to_offset(lsid) - lsid_local.
 */

/**
 * Super block data of the log device.
 *
 * sizeof(walb_super_sector_t) must be <= sector_size.
 */
typedef struct walb_super_sector {

        /* 4 * 4 + 16 + 8 * 5 = 72 bytes */
        
        /* Check sum of the super block */
        u32 checksum;

        /* Atomic read/write size.
           Normally physical block size of the device.
           Physical block size of the log device and data device
           must be the same. */
        u32 sector_size;
        
        /* Number of sectors for snapshot metadata. */
        u32 snapshot_metadata_size;
        u32 reserved1;

        /* UUID of the wal device. */
        u8 uuid[16];

        /* Offset of the oldest log record inside ring buffer.
           [sector] */
        u64 start_offset;

        /* Ring buffer size [sector] */
        u64 ring_buffer_size;
        
        /* Log sequence id of the oldest log record in the ring buffer. */
        u64 oldest_lsid;
        
        /* Log sequence id of the latest log record written to the data device also.
           This is used for checkpointing.
           When walb device is assembled redo must be
           from written_lsid to the latest lsid stored in the log device. */
        u64 written_lsid;

        /* Size of wrapper block device [sector] */
        u64 device_size;
        
} __attribute__((packed)) walb_super_sector_t;

/**
 * Each snapshot information.
 */
typedef struct walb_snapshot_record {

        /* 8 + 8 + 64 = 80 bytes */
        
        u64 lsid;
        u64 timestamp; /* in seconds (the same as 'time' system call output). */
        u8 name[64]; /* '\0' means end of string */
        
} __attribute__((packed)) walb_snapshot_record_t;

/**
 * Snapshot data inside sector.
 *
 * sizeof(walb_snapshot_sector_t) <= walb_super_sector.sector_size
 */
typedef struct walb_snapshot_sector {

        /* Checksum of snapshot sector */
        u32 checksum;

        /* Allocation bitmap of the continuous records
           stored in the sector.
           (i + 1)'th record exists when (bitmap & (1 << i)) != 0.
        */
        u32 bitmap;

        walb_snapshot_record_t record[0];
        /* The continuous data have records.
           The number of records is up to 32 or sector size */
        
} __attribute__((packed)) walb_snapshot_sector_t;

/**
 * Number of snapshots in a sector.
 */
static inline int max_n_snapshots_in_sector(int sector_size)
{
        int size;

#if 0
        printf("walb_snapshot_sector_t size: %zu\n",
               sizeof(walb_snapshot_sector_t));
        printf("walb_snapshot_record_t size: %zu\n",
               sizeof(walb_snapshot_record_t));
#endif
        
        size = (sector_size - sizeof(walb_snapshot_sector_t))
                / sizeof(walb_snapshot_record_t);
        
        /* It depends on bitmap length. */
        return (size < 32 ? size : 32);
}


/**
 * Get metadata size
 *
 * @sector_size sector size.
 * @n_snapshots number snapshot to keep.
 * 
 * @return required metadata size by the sector.
 */
static inline int get_metadata_size(int sector_size, int n_snapshots)
{
        ASSERT(PAGE_SIZE % sector_size == 0 &&
               PAGE_SIZE >= sector_size);
        
        int n_sectors;
        int t = max_n_snapshots_in_sector(sector_size);
        n_sectors = (n_snapshots + t - 1) / t;
        return n_sectors;
}

/**
 * Get offset of primary super sector.
 *
 * @sector_size sector size in bytes.
 * @return offset in sectors.
 */
static inline u64 get_super_sector0_offset(int sector_size)
{
        ASSERT(PAGE_SIZE % sector_size == 0);
        return PAGE_SIZE/sector_size; /* skip reserved page */
}

/**
 * Get offset of first metadata sector.
 *
 * @sector_size sector size in bytes.
 * @return offset in sectors.
 */
static inline u64 get_metadata_offset(int sector_size)
{
        return get_super_sector0_offset(sector_size) + 1;
}

/**
 * Get offset of secondary super sector.
 *
 * @sector_size sector size in bytes.
 * @n_snapshots number of snapshot to keep.
 * @return offset in sectors.
 */
static inline u64 get_super_sector1_offset(int sector_size, int n_snapshots)
{
        return  get_metadata_offset(sector_size) +
                get_metadata_size(sector_size, n_snapshots);
}

/**
 * Get ring buffer offset.
 *
 * @sector_size sector size.
 * @n_snapshots number of snapshot to keep.
 *
 * @return ring buffer offset by the sector.
 */
static inline u64 get_ring_buffer_offset(int sector_size, int n_snapshots)
{
        return  get_super_sector1_offset(sector_size, n_snapshots) + 1;
}


/**
 * Get offset of primary super sector.
 */
static inline u64 get_super_sector0_offset_2(const walb_super_sector_t* super_sect)
{
        ASSERT(super_sect != NULL);
        return get_super_sector0_offset(super_sect->sector_size);
}

/**
 * Get offset of first metadata sector.
 */
static inline u64 get_metadata_offset_2(const walb_super_sector_t* super_sect)
{
        ASSERT(super_sect != NULL);
        return get_metadata_offset(super_sect->sector_size);
}

/**
 * Get offset of secondary super sector.
 */
static inline u64 get_super_sector1_offset_2(const walb_super_sector_t* super_sect)
{
        ASSERT(super_sect != NULL);
        return  get_metadata_offset(super_sect->sector_size) +
                super_sect->snapshot_metadata_size;
}

/**
 * Get ring buffer offset.
 */
static inline u64 get_ring_buffer_offset_2(const walb_super_sector_t* super_sect)
{
        ASSERT(super_sect != NULL);
        return  get_super_sector1_offset_2(super_sect) + 1;
}



#endif /* WALB_LOG_DEVICE_H */
