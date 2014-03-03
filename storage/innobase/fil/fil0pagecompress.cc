/*****************************************************************************

Copyright (C) 2013, 2014, SkySQL Ab. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/

/******************************************************************//**
@file fil/fil0pagecompress.cc
Implementation for page compressed file spaces.

Created 11/12/2013 Jan Lindström jan.lindstrom@skysql.com
***********************************************************************/

#include "fil0fil.h"
#include "fil0pagecompress.h"

#include <debug_sync.h>
#include <my_dbug.h>

#include "mem0mem.h"
#include "hash0hash.h"
#include "os0file.h"
#include "mach0data.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "log0recv.h"
#include "fsp0fsp.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "dict0dict.h"
#include "page0page.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "row0mysql.h"
#ifndef UNIV_HOTBACKUP
# include "buf0lru.h"
# include "ibuf0ibuf.h"
# include "sync0sync.h"
# include "os0sync.h"
#else /* !UNIV_HOTBACKUP */
# include "srv0srv.h"
static ulint srv_data_read, srv_data_written;
#endif /* !UNIV_HOTBACKUP */
#include "zlib.h"
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/falloc.h>
#endif
#include "row0mysql.h"
#ifdef HAVE_LZ4
#include "lz4.h"
#endif

/****************************************************************//**
For page compressed pages compress the page before actual write
operation.
@return compressed page to be written*/
byte*
fil_compress_page(
/*==============*/
	ulint		space_id,      /*!< in: tablespace id of the
				       table. */
	byte*           buf,           /*!< in: buffer from which to write; in aio
				       this must be appropriately aligned */
        byte*           out_buf,       /*!< out: compressed buffer */
        ulint           len,           /*!< in: length of input buffer.*/
        ulint           compression_level, /* in: compression level */
	ulint*          out_len)       /*!< out: actual length of compressed page */
{
        int err = Z_OK;
        int level = 0;
        ulint header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE;
	ulint write_size=0;

	ut_ad(buf);
	ut_ad(out_buf);
	ut_ad(len);
	ut_ad(out_len);

        level = compression_level;
	ut_ad(fil_space_is_page_compressed(space_id));

	fil_system_enter();
	fil_space_t* space = fil_space_get_by_id(space_id);
	fil_system_exit();

	/* If no compression level was provided to this table, use system
	default level */
	if (level == 0) {
		level = page_zip_level;
	}

#ifdef UNIV_DEBUG
	fprintf(stderr,
		"InnoDB: Note: Preparing for compress for space %lu name %s len %lu\n",
		space_id, fil_space_name(space), len);
#endif /* UNIV_DEBUG */

	write_size = UNIV_PAGE_SIZE - header_len;

#ifdef HAVE_LZ4
	if (srv_use_lz4) {
		err = LZ4_compress_limitedOutput((const char *)buf, (char *)out_buf+header_len, len, write_size);
		write_size = err;

		if (err == 0) {
			/* If error we leave the actual page as it was */

			fprintf(stderr,
				"InnoDB: Warning: Compression failed for space %lu name %s len %lu rt %d write %lu\n",
				space_id, fil_space_name(space), len, err, write_size);

			*out_len = len;
			return (buf);
		}
	} else {
#endif /* HAVE_LZ4 */
		err = compress2(out_buf+header_len, &write_size, buf, len, level);

		if (err != Z_OK) {
			/* If error we leave the actual page as it was */

			fprintf(stderr,
				"InnoDB: Warning: Compression failed for space %lu name %s len %lu rt %d write %lu\n",
				space_id, fil_space_name(space), len, err, write_size);

			*out_len = len;
			return (buf);
		}
#ifdef HAVE_LZ4
	}
#endif /* HAVE_LZ4 */

	/* Set up the page header */
	memcpy(out_buf, buf, FIL_PAGE_DATA);
	/* Set up the checksum */
	mach_write_to_4(out_buf+FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);
	/* Set up the correct page type */
	mach_write_to_2(out_buf+FIL_PAGE_TYPE, FIL_PAGE_PAGE_COMPRESSED);
	/* Set up the flush lsn to be compression algorithm */

#ifdef HAVE_LZ4
	if (srv_use_lz4) {
		mach_write_to_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN, FIL_PAGE_COMPRESSION_LZ4);
	} else {
#endif /* HAVE_LZ4 */
		mach_write_to_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN, FIL_PAGE_COMPRESSION_ZLIB);

#ifdef HAVE_LZ4
	}
#endif /* HAVE_LZ4 */

	/* Set up the actual payload lenght */
	mach_write_to_2(out_buf+FIL_PAGE_DATA, write_size);

#ifdef UNIV_DEBUG
	/* Verify */
	ut_ad(fil_page_is_compressed(out_buf));
	ut_ad(mach_read_from_4(out_buf+FIL_PAGE_SPACE_OR_CHKSUM) == BUF_NO_CHECKSUM_MAGIC);
	ut_ad(mach_read_from_2(out_buf+FIL_PAGE_DATA) == write_size);

#ifdef HAVE_LZ4
	if (srv_use_lz4) {
		ut_ad(mach_read_from_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN) == FIL_PAGE_COMPRESSION_LZ4);
	} else {
#endif /* HAVE_LZ4 */
		ut_ad(mach_read_from_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN) == FIL_PAGE_COMPRESSION_ZLIB);

#ifdef HAVE_LZ4
	}
#endif /* HAVE_LZ4 */
#endif /* UNIV_DEBUG */

	write_size+=header_len;

#define SECT_SIZE 512

	/* Actual write needs to be alligned on block size */
	if (write_size % SECT_SIZE) {
		write_size = (write_size + SECT_SIZE-1) & ~(SECT_SIZE-1);
		ut_a((write_size % SECT_SIZE) == 0);
	}

#ifdef UNIV_DEBUG
	fprintf(stderr,
		"InnoDB: Note: Compression succeeded for space %lu name %s len %lu out_len %lu\n",
		space_id, fil_space_name(space), len, write_size);
#endif


	srv_stats.page_compression_saved.add((len - write_size));
	if ((len - write_size) > 0) {
		srv_stats.page_compression_trim_sect512.add(((len - write_size) / SECT_SIZE));
		srv_stats.page_compression_trim_sect4096.add(((len - write_size) / (SECT_SIZE*8)));
	}
	//srv_stats.page_compressed_trim_op.inc();
	srv_stats.pages_page_compressed.inc();
	*out_len = write_size;

	return(out_buf);

}

/****************************************************************//**
For page compressed pages decompress the page after actual read
operation. */
void
fil_decompress_page(
/*================*/
	byte*           page_buf,      /*!< in: preallocated buffer or NULL */
	byte*           buf,           /*!< out: buffer from which to read; in aio
				       this must be appropriately aligned */
        ulint           len)           /*!< in: length of output buffer.*/
{
        int err = 0;
        ulint actual_size = 0;
	ulint compression_alg = 0;
	byte *in_buf;

	ut_ad(buf);
	ut_ad(len);

	/* Before actual decompress, make sure that page type is correct */

	if (mach_read_from_4(buf+FIL_PAGE_SPACE_OR_CHKSUM) != BUF_NO_CHECKSUM_MAGIC ||
		mach_read_from_2(buf+FIL_PAGE_TYPE) != FIL_PAGE_PAGE_COMPRESSED) {
		fprintf(stderr,
			"InnoDB: Corruption: We try to uncompress corrupted page\n"
			"InnoDB: CRC %lu type %lu.\n"
			"InnoDB: len %lu\n",
			mach_read_from_4(buf+FIL_PAGE_SPACE_OR_CHKSUM),
			mach_read_from_2(buf+FIL_PAGE_TYPE), len);

		fflush(stderr);
		ut_error;
	}

	/* Get compression algorithm */
	compression_alg = mach_read_from_8(buf+FIL_PAGE_FILE_FLUSH_LSN);

	// If no buffer was given, we need to allocate temporal buffer
	if (page_buf == NULL) {
#ifdef UNIV_DEBUG
		fprintf(stderr,
			"InnoDB: Note: FIL: Compression buffer not given, allocating...\n");
#endif /* UNIV_DEBUG */
		in_buf = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE));
	} else {
		in_buf = page_buf;
	}

	/* Get the actual size of compressed page */
	actual_size = mach_read_from_2(buf+FIL_PAGE_DATA);
	/* Check if payload size is corrupted */
	if (actual_size == 0 || actual_size > UNIV_PAGE_SIZE) {
		fprintf(stderr,
			"InnoDB: Corruption: We try to uncompress corrupted page\n"
			"InnoDB: actual size %lu compression %s\n",
			actual_size, fil_get_compression_alg_name(compression_alg));
		fflush(stderr);
		ut_error;
	}

	if (compression_alg == FIL_PAGE_COMPRESSION_ZLIB) {

#ifdef UNIV_DEBUG
		fprintf(stderr,
			"InnoDB: Note: Preparing for decompress for len %lu\n",
			actual_size);
#endif /* UNIV_DEBUG */

		err= uncompress(in_buf, &len, buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE, (unsigned long)actual_size);


		/* If uncompress fails it means that page is corrupted */
		if (err != Z_OK) {

			fprintf(stderr,
				"InnoDB: Corruption: Page is marked as compressed\n"
				"InnoDB: but uncompress failed with error %d.\n"
				"InnoDB: size %lu len %lu\n",
				err, actual_size, len);

			fflush(stderr);

			ut_error;
		}

#ifdef UNIV_DEBUG
		fprintf(stderr,
			"InnoDB: Note: Decompression succeeded for len %lu \n",
			len);
#endif /* UNIV_DEBUG */
#ifdef HAVE_LZ4
	} else if (compression_alg == FIL_PAGE_COMPRESSION_LZ4) {
		err = LZ4_decompress_fast((const char *)buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE, (char *)in_buf, UNIV_PAGE_SIZE);

		if (err != (int)actual_size) {
			fprintf(stderr,
				"InnoDB: Corruption: Page is marked as compressed\n"
				"InnoDB: but decompression read only %d bytes.\n"
				"InnoDB: size %lu len %lu\n",
				err, actual_size, len);
			fflush(stderr);

			ut_error;
		}
#endif /* HAVE_LZ4 */
	} else {
		fprintf(stderr,
			"InnoDB: Corruption: Page is marked as compressed\n"
			"InnoDB: but compression algorithm %s\n"
			"InnoDB: is not known.\n"
			,fil_get_compression_alg_name(compression_alg));

		fflush(stderr);
		ut_error;
	}

	srv_stats.pages_page_decompressed.inc();

	/* Copy the uncompressed page to the buffer pool, not
	really any other options. */
	memcpy(buf, in_buf, len);

	// Need to free temporal buffer if no buffer was given
	if (page_buf == NULL) {
		ut_free(in_buf);
	}
}

