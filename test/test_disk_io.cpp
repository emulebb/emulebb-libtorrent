/*

Copyright (c) 2024, Arvid Norberg
All rights reserved.

You may use, distribute and modify this code under the terms of the BSD license,
see LICENSE file.
*/

#include <iostream>
#include "test.hpp"
#include "setup_transfer.hpp"
#include "test_utils.hpp"
#include "libtorrent/disk_interface.hpp"
#include "libtorrent/mmap_disk_io.hpp"
#include "libtorrent/posix_disk_io.hpp"
#include "libtorrent/pread_disk_io.hpp"
#include "libtorrent/session_params.hpp" // for disk_io_constructor_type
#include "libtorrent/settings_pack.hpp" // for default_settings
#include "libtorrent/flags.hpp"
#include "libtorrent/storage_defs.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/performance_counters.hpp"
#include "libtorrent/file_storage.hpp"
#include "libtorrent/aux_/vector.hpp"
#include "libtorrent/aux_/time.hpp"
#include "libtorrent/sha1_hash.hpp"
#include "libtorrent/hasher.hpp"

using disk_test_mode_t = lt::flags::bitfield_flag<std::uint32_t, struct disk_test_mode_tag>;

namespace test_mode {
	using lt::operator ""_bit;
constexpr disk_test_mode_t v1 = 0_bit;
constexpr disk_test_mode_t v2 = 1_bit;
}

namespace {
	void disk_io_test_suite_impl(lt::disk_io_constructor_type disk_io,
		disk_test_mode_t const flags,
		int const piece_size,
		int const num_files,
		int const hasher_threads,
		int const disk_threads,
		bool const raise_fence)
	{
		lt::io_context ios;
		lt::counters cnt;
		lt::settings_pack sett = lt::default_settings();
		sett.set_int(lt::settings_pack::hashing_threads, hasher_threads);
		sett.set_int(lt::settings_pack::aio_threads, disk_threads);
		std::unique_ptr<lt::disk_interface> disk_thread = disk_io(ios, sett, cnt);

		std::cout << "disk_io_test_suite: " << ((flags & test_mode::v1) ? "v1 " : "")
				  << ((flags & test_mode::v2) ? "v2 " : "") << " disk-threads: " << disk_threads
				  << " hasher_threads: " << hasher_threads << " num-files: " << num_files
				  << " piece_size: " << piece_size << std::endl;

		lt::file_storage fs;
		fs.set_piece_length(piece_size);
		std::int64_t total_size = 0;
		for (int i = 0; i < num_files; ++i)
		{
			int const file_size = piece_size * 2 + i * 11;
			total_size += file_size;
			fs.add_file("test-torrent/file-" + std::to_string(i), file_size, {});
		}
		fs.set_num_pieces(int((total_size + piece_size - 1) / piece_size));

		lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities;
		std::string const name = "test_torrent_store";
		lt::renamed_files rf;
		lt::storage_params params{
			fs,
			rf,
			name,
			{},
			lt::storage_mode_t::storage_mode_sparse,
			priorities,
			lt::sha1_hash{},
			bool(flags & test_mode::v1),
			bool(flags & test_mode::v2),
		};

		lt::storage_holder storage = disk_thread->new_torrent(params, std::shared_ptr<void>());

		int blocks_written = 0;
		int expect_written = 0;
		int hashes_done = 0;
		int expect_hashes = 0;
		int clears_done = 0;
		int expect_clears = 0;
		bool const need_v1 = bool(flags & test_mode::v1);
		bool const need_v2 = bool(flags & test_mode::v2);
		int const block_size = std::min(lt::default_block_size, piece_size);
		for (lt::piece_index_t p : fs.piece_range())
		{
			int const len = need_v1 ? fs.piece_size(p) : fs.piece_size2(p);
			std::vector<char> const buffer = generate_piece(p, len);

			// Raise a per-storage fence right before this piece's writes. Clearing
			// p while it is still empty is a no-op clear, but it raises the fence,
			// so the writes (and the hash) below are queued behind it and only
			// inserted/run when the clear lowers it. If the fence machinery ran the
			// hash before the queued writes landed in the cache, the hash would not
			// match. We deliberately don't submit_jobs() until the whole piece is
			// queued, so the fence stays up across all of p's writes.
			if (raise_fence)
			{
				disk_thread->async_clear_piece(
					storage, p, [&clears_done](lt::piece_index_t) { ++clears_done; });
				++expect_clears;
			}
			for (int block = 0; block < len; block += block_size)
			{
				int const write_size = std::min(block_size, len - block);
				bool const last_block = (block + block_size >= len);
				lt::disk_job_flags_t const disk_flags =
					last_block ? lt::disk_interface::flush_piece : lt::disk_job_flags_t{};
				disk_thread->async_write(
					storage,
					lt::peer_request{p, block, write_size},
					buffer.data() + block,
					std::shared_ptr<lt::disk_observer>(),
					[&](lt::storage_error const& e) {
						if (e.ec)
						{
							std::cout << "ERROR: failed to write block (p: " << p << " b: " << block
									  << " s: " << write_size << "): (" << e.ec.value() << ") "
									  << e.ec.message() << std::endl;
							std::abort();
						}
						++blocks_written;
					},
					disk_flags);
				++expect_written;

				if (last_block)
				{
					// Issuing async_hash after a full piece is written ensures the
					// disk cache flushes the blocks (flush only happens once hashing
					// completes).
					int const blocks_in_piece = (len + block_size - 1) / block_size;
					auto v2_hashes = need_v2 ? std::make_shared<std::vector<lt::sha256_hash>>(
												   std::size_t(blocks_in_piece))
											 : std::make_shared<std::vector<lt::sha256_hash>>();
					lt::disk_job_flags_t const hash_flags =
						need_v1 ? lt::disk_interface::v1_hash : lt::disk_job_flags_t{};
					// expected hashes computed from the same generate_piece data.
					// v2 pieces don't span file boundaries, so v2 hashes only cover
					// the v2 portion (up to piece_size2) of the block buffer.
					std::vector<lt::sha256_hash> expected_v2;
					lt::sha1_hash expected_v1;
					if (need_v2)
					{
						expected_v2.resize(std::size_t(blocks_in_piece));
						int const v2_blocks = fs.blocks_in_piece2(p);
						int const v2_size = fs.piece_size2(p);
						for (int b = 0; b < v2_blocks; ++b)
						{
							int const off = b * block_size;
							int const bsize = std::min(block_size, v2_size - off);
							lt::hasher256 hh;
							hh.update({buffer.data() + off, bsize});
							expected_v2[std::size_t(b)] = hh.final();
						}
					}
					if (need_v1)
					{
						lt::hasher hh;
						hh.update({buffer.data(), len});
						expected_v1 = hh.final();
					}

					disk_thread->async_hash(storage,
						p,
						lt::span<lt::sha256_hash>(*v2_hashes),
						hash_flags | lt::disk_interface::flush_piece,
						[&hashes_done,
							p,
							v2_hashes,
							need_v1,
							need_v2,
							expected_v2 = std::move(expected_v2),
							expected_v1](lt::piece_index_t,
							lt::sha1_hash const& v1_hash,
							lt::storage_error const& e) {
							if (e.ec)
							{
								std::cout << "ERROR: failed to hash piece (p: " << p << "): ("
										  << e.ec.value() << ") " << e.ec.message() << std::endl;
								std::abort();
							}
							if (need_v2)
							{
								TEST_EQUAL(v2_hashes->size(), expected_v2.size());
								for (std::size_t i = 0; i < expected_v2.size(); ++i)
									TEST_CHECK((*v2_hashes)[i] == expected_v2[i]);
							}
							if (need_v1) TEST_CHECK(v1_hash == expected_v1);
							++hashes_done;
						});
					++expect_hashes;
				}

				// in fence mode, keep the fence up across all of the piece's writes
				// by deferring submission until the whole piece (and its hash) is
				// queued.
				if (!raise_fence || last_block) disk_thread->submit_jobs();
			}
		}

		auto const start_time = lt::aux::time_now();
#ifdef TORRENT_SIMULATE_SLOW_HASH
		auto const timeout = lt::seconds(30);
#else
		auto const timeout = lt::seconds(20);
#endif
		while (blocks_written < expect_written || hashes_done < expect_hashes
			|| clears_done < expect_clears)
		{
			ios.run_for(std::chrono::seconds(1));
			std::cout << "blocks_written: " << blocks_written << "/" << expect_written
					  << " hashes_done: " << hashes_done << "/" << expect_hashes
					  << " clears_done: " << clears_done << "/" << expect_clears << std::endl;

			if (lt::aux::time_now() - start_time > timeout)
			{
				TEST_ERROR("timeout");
				break;
			}
		}

		TEST_EQUAL(blocks_written, expect_written);
		TEST_EQUAL(hashes_done, expect_hashes);
		TEST_EQUAL(clears_done, expect_clears);

		disk_thread->abort(true);
	}

	void disk_io_test_suite(
		lt::disk_io_constructor_type disk_io, int const num_files, bool const raise_fence = false)
	{
		for (disk_test_mode_t flags : {test_mode::v1, test_mode::v2, test_mode::v1 | test_mode::v2})
		{
			for (int hasher_threads : {0, 2})
			{
				for (int disk_threads : {0, 2})
				{
					for (int piece_size : {300, 0x8000})
					{
						disk_io_test_suite_impl(disk_io,
							flags,
							piece_size,
							num_files,
							hasher_threads,
							disk_threads,
							raise_fence);
					}
				}
			}
		}
	}

	// Clears pieces that have blocks already flushed to disk. A small
	// max_queued_disk_bytes forces the cache to flush under pressure as we write, so
	// by the time we clear, a piece's blocks are a mix of flushed (disk_buffer_ref)
	// and still-pending write jobs -- exercising clear_piece_impl's handling of both
	// (validated by the cache's invariant checks). pread_disk_io only.
	void clear_flushed_impl(disk_test_mode_t const flags,
		int const piece_size,
		int const disk_threads,
		int const hasher_threads,
		int const cache_blocks)
	{
		lt::io_context ios;
		lt::counters cnt;
		lt::settings_pack sett = lt::default_settings();
		sett.set_int(lt::settings_pack::hashing_threads, hasher_threads);
		sett.set_int(lt::settings_pack::aio_threads, disk_threads);
		sett.set_int(
			lt::settings_pack::max_queued_disk_bytes, cache_blocks * lt::default_block_size);
		std::unique_ptr<lt::disk_interface> disk_thread =
			lt::pread_disk_io_constructor(ios, sett, cnt);

		std::cout << "clear_flushed: " << ((flags & test_mode::v1) ? "v1 " : "")
				  << ((flags & test_mode::v2) ? "v2 " : "") << " disk-threads: " << disk_threads
				  << " hasher_threads: " << hasher_threads << " piece_size: " << piece_size
				  << " cache_blocks: " << cache_blocks << std::endl;

		lt::file_storage fs;
		fs.set_piece_length(piece_size);
		std::int64_t total_size = 0;
		for (int i = 0; i < 4; ++i)
		{
			int const file_size = piece_size * 3 + i * 11;
			total_size += file_size;
			fs.add_file("test-torrent/file-" + std::to_string(i), file_size, {});
		}
		fs.set_num_pieces(int((total_size + piece_size - 1) / piece_size));

		lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities;
		lt::renamed_files rf;
		lt::storage_params params{fs,
			rf,
			"test_torrent_store",
			{},
			lt::storage_mode_t::storage_mode_sparse,
			priorities,
			lt::sha1_hash{},
			bool(flags & test_mode::v1),
			bool(flags & test_mode::v2)};
		lt::storage_holder storage = disk_thread->new_torrent(params, std::shared_ptr<void>());

		int written = 0;
		int expect_written = 0;
		int clears_done = 0;
		int expect_clears = 0;
		bool const need_v1 = bool(flags & test_mode::v1);
		int const block_size = std::min(lt::default_block_size, piece_size);

		for (lt::piece_index_t p : fs.piece_range())
		{
			int const len = need_v1 ? fs.piece_size(p) : fs.piece_size2(p);
			std::vector<char> const buffer = generate_piece(p, len);
			for (int block = 0; block < len; block += block_size)
			{
				int const write_size = std::min(block_size, len - block);
				disk_thread->async_write(
					storage,
					lt::peer_request{p, block, write_size},
					buffer.data() + block,
					std::shared_ptr<lt::disk_observer>(),
					[&written](lt::storage_error const& e) {
						// success == the block was flushed; operation_aborted == it
						// was still pending when the clear below dropped it.
						if (e.ec && e.ec != boost::asio::error::operation_aborted) std::abort();
						if (!e.ec) ++written;
					},
					lt::disk_job_flags_t{});
				++expect_written;
			}
			disk_thread->submit_jobs();
		}

		// let flushing make progress, so the pieces we clear have flushed blocks.
		auto const start_time = lt::aux::time_now();
		while (written < expect_written / 2 && lt::aux::time_now() - start_time < lt::seconds(15))
			ios.run_for(std::chrono::milliseconds(50));

		for (lt::piece_index_t p : fs.piece_range())
		{
			disk_thread->async_clear_piece(
				storage, p, [&clears_done](lt::piece_index_t) { ++clears_done; });
			++expect_clears;
		}
		disk_thread->submit_jobs();

		while (clears_done < expect_clears && lt::aux::time_now() - start_time < lt::seconds(30))
			ios.run_for(std::chrono::seconds(1));

		TEST_EQUAL(clears_done, expect_clears);
		TEST_CHECK(written > 0);

		disk_thread->abort(true);
	}

	void clear_flushed_suite()
	{
		// hasher_threads is kept >= 1: with 0 hasher threads, clearing an unhashed
		// v2 piece parks the clear on v2_pending and nothing drains it (no further
		// writes), which is a separate edge unrelated to this coverage. The cache
		// holds a few pieces, so as we write all of them earlier pieces flush to
		// disk; clearing them then hits flushed buffers (and any still-pending
		// write jobs) in clear_piece_impl.
		for (disk_test_mode_t flags : {test_mode::v1, test_mode::v2, test_mode::v1 | test_mode::v2})
			for (int disk_threads : {0, 2})
				clear_flushed_impl(flags, 0x8000, disk_threads, 2, 8);
	}

	// Verify that async_hash2 returns the correct SHA-256 for a block that has
	// been written but is still sitting in the disk cache (for pread_disk_io) /
	// store buffer (for mmap_disk_io). With hashing_threads=0 the hasher kick is
	// disabled, so no precomputed v2 block hash is stashed on the storage. With
	// aio_threads=0 there is no thread to flush the cache to disk. In this state
	// any hash2 implementation that falls through to a disk read would read zeros
	// (the block has not been written to disk yet) and produce a wrong hash.
	void hash2_before_flush_impl(
		lt::disk_io_constructor_type disk_io, disk_test_mode_t const flags, int const piece_size)
	{
		lt::io_context ios;
		lt::counters cnt;
		lt::settings_pack sett = lt::default_settings();
		sett.set_int(lt::settings_pack::hashing_threads, 0);
		sett.set_int(lt::settings_pack::aio_threads, 0);
		std::unique_ptr<lt::disk_interface> disk_thread = disk_io(ios, sett, cnt);

		std::cout << "hash2_before_flush: " << ((flags & test_mode::v1) ? "v1 " : "")
				  << ((flags & test_mode::v2) ? "v2 " : "") << " piece_size: " << piece_size
				  << std::endl;

		lt::file_storage fs;
		fs.set_piece_length(piece_size);
		int const file_size = piece_size * 3 + 17;
		fs.add_file("hash2_before_flush_torrent/file-0", file_size, {});
		fs.set_num_pieces(int((file_size + piece_size - 1) / piece_size));

		lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities;
		std::string const name = "hash2_before_flush_store";
		lt::renamed_files rf;
		lt::storage_params params{
			fs,
			rf,
			name,
			{},
			lt::storage_mode_t::storage_mode_sparse,
			priorities,
			lt::sha1_hash{},
			bool(flags & test_mode::v1),
			bool(flags & test_mode::v2),
		};

		lt::storage_holder storage = disk_thread->new_torrent(params, std::shared_ptr<void>());

		int const block_size = std::min(lt::default_block_size, piece_size);
		bool const need_v1 = bool(flags & test_mode::v1);

		// only counts hash2 callbacks: write callbacks won't fire here because
		// nothing flushes the cache to disk.
		int hashes_done = 0;
		int hashes_expected = 0;
		bool any_mismatch = false;

		for (lt::piece_index_t p : fs.piece_range())
		{
			int const piece_size_v2 = fs.piece_size2(p);
			int const len = need_v1 ? fs.piece_size(p) : piece_size_v2;
			auto buffer = std::make_shared<std::vector<char>>(generate_piece(p, len));
			for (int off = 0; off < len; off += block_size)
			{
				int const write_size = std::min(block_size, len - off);
				disk_thread->async_write(
					storage,
					lt::peer_request{p, off, write_size},
					buffer->data() + off,
					std::shared_ptr<lt::disk_observer>(),
					[buffer](lt::storage_error const&) {
						// hold the buffer alive while a write is outstanding;
						// no other action needed
					},
					lt::disk_job_flags_t{});

				// only verify blocks inside the v2 piece (v2 pieces don't cross
				// file boundaries, so v1 padding past piece_size2 is not covered).
				if (off >= piece_size_v2) continue;

				int const v2_size = std::min(block_size, piece_size_v2 - off);
				lt::hasher256 hh;
				hh.update({buffer->data() + off, v2_size});
				lt::sha256_hash const expected = hh.final();

				disk_thread->async_hash2(storage,
					p,
					off,
					lt::disk_job_flags_t{},
					[&hashes_done, &any_mismatch, expected, p, off](lt::piece_index_t,
						lt::sha256_hash const& hash,
						lt::storage_error const& e) {
						if (e.ec)
						{
							std::cout << "ERROR: failed to hash2 (p: " << p << " off: " << off
									  << "): (" << e.ec.value() << ") " << e.ec.message()
									  << std::endl;
							std::abort();
						}
						if (hash != expected)
						{
							std::cout << "MISMATCH at piece " << p << " offset " << off
									  << ": expected " << expected << " got " << hash << std::endl;
							any_mismatch = true;
						}
						++hashes_done;
					});
				++hashes_expected;

				disk_thread->submit_jobs();
			}
		}

		// also drain any cached blocks so the disk_io destructor's m_cache.size()
		// == 0 assert is happy. clear_piece aborts the queued write jobs (their
		// callbacks fire with cancel) and drops the cached buffers. The hash2
		// results were computed inline above, so issuing the clears now does not
		// affect the values the hash2 callbacks will deliver.
		int cleared = 0;
		int const num_pieces = static_cast<int>(fs.num_pieces());
		for (lt::piece_index_t p : fs.piece_range())
		{
			disk_thread->async_clear_piece(
				storage, p, [&cleared](lt::piece_index_t) { ++cleared; });
		}
		disk_thread->submit_jobs();

		auto const start_time = lt::aux::time_now();
		auto const timeout = lt::seconds(20);
		while (hashes_done < hashes_expected || cleared < num_pieces)
		{
			ios.run_for(std::chrono::seconds(1));
			if (lt::aux::time_now() - start_time > timeout)
			{
				TEST_ERROR("timeout");
				break;
			}
		}

		TEST_EQUAL(hashes_done, hashes_expected);
		TEST_EQUAL(cleared, num_pieces);
		TEST_CHECK(!any_mismatch);

		disk_thread->abort(true);
	}

	void hash2_before_flush_suite(lt::disk_io_constructor_type disk_io)
	{
		for (disk_test_mode_t flags : {test_mode::v2, test_mode::v1 | test_mode::v2})
		{
			for (int piece_size : {0x4000, 0x8000})
			{
				hash2_before_flush_impl(disk_io, flags, piece_size);
			}
		}
	}
}

#if TORRENT_HAVE_MMAP || TORRENT_HAVE_MAP_VIEW_OF_FILE
TORRENT_TEST(test_mmap_disk_io)
{
	disk_io_test_suite(&lt::mmap_disk_io_constructor, 3);
}

#endif

TORRENT_TEST(test_posix_disk_io)
{
	disk_io_test_suite(&lt::posix_disk_io_constructor, 3);
}

TORRENT_TEST(test_pread_disk_io)
{
	disk_io_test_suite(&lt::pread_disk_io_constructor, 3);
}

// same as above, but raises a clear_piece fence right before each piece's
// writes, exercising the path where writes are queued behind a fence and must
// be inserted into the cache before the piece's hash job runs.
TORRENT_TEST(test_pread_disk_io_fence)
{
	disk_io_test_suite(&lt::pread_disk_io_constructor, 3, true);
}

// clears pieces whose blocks have been flushed to disk (tiny cache), exercising
// clear_piece_impl on flushed buffers and pending write jobs together.
TORRENT_TEST(test_pread_disk_io_clear_flushed) { clear_flushed_suite(); }

#if TORRENT_HAVE_MMAP || TORRENT_HAVE_MAP_VIEW_OF_FILE
TORRENT_TEST(test_mmap_disk_io_hash2_before_flush)
{
	hash2_before_flush_suite(&lt::mmap_disk_io_constructor);
}

#endif

TORRENT_TEST(test_posix_disk_io_hash2_before_flush)
{
	hash2_before_flush_suite(&lt::posix_disk_io_constructor);
}

TORRENT_TEST(test_pread_disk_io_hash2_before_flush)
{
	hash2_before_flush_suite(&lt::pread_disk_io_constructor);
}
