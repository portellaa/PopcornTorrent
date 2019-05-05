/*

Copyright (c) 2010-2018, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef TORRENT_BLOCK_CACHE
#define TORRENT_BLOCK_CACHE

#include <cstdint>
#include <list>
#include <vector>
#include <unordered_set>
#include <array>

#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/intrusive/list.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"

#include "libtorrent/time.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/io_service_fwd.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/tailqueue.hpp"
#include "libtorrent/linked_list.hpp"
#include "libtorrent/disk_buffer_pool.hpp"
#include "libtorrent/aux_/storage_utils.hpp" // for iovec_t
#include "libtorrent/disk_io_job.hpp"
#include "libtorrent/aux_/unique_ptr.hpp"
#if TORRENT_USE_ASSERTS
#include "libtorrent/aux_/vector.hpp"
#endif

namespace libtorrent {

	struct disk_io_job;
	struct storage_interface;
	struct cache_status;
	struct counters;
namespace aux {

		struct session_settings;
		struct block_cache_reference;
	}
#if TORRENT_USE_ASSERTS
	class file_storage;
#endif

#if TORRENT_USE_ASSERTS || !defined TORRENT_DISABLE_LOGGING

	struct piece_log_t
	{
		explicit piece_log_t(job_action_t j, int b = -1): job(j), block(b) {}
		job_action_t job;
		int block;

		// these are "jobs" thar cause piece_refcount
		// to be incremented
		enum artificial_jobs
		{
			flushing = static_cast<int>(job_action_t::num_job_ids), // 20
			flush_expired,
			try_flush_write_blocks,
			try_flush_write_blocks2,
			flush_range,
			clear_outstanding_jobs,
			set_outstanding_jobs,

			last_job
		};
		explicit piece_log_t(artificial_jobs j, int b = -1): job(static_cast<job_action_t>(j)), block(b) {}

		static std::array<char const*, 7> const job_names;
	};

	char const* job_name(job_action_t j);

#endif // TORRENT_DISABLE_LOGGING

#if TORRENT_USE_ASSERTS
	void print_piece_log(aux::vector<piece_log_t> const& piece_log);
	void assert_print_piece(cached_piece_entry const* pe);
#endif

	extern std::array<const char*, 15> const job_action_name;

	struct TORRENT_EXTRA_EXPORT partial_hash
	{
		partial_hash(): offset(0) {}
		// the number of bytes in the piece that has been hashed
		int offset;
		// the SHA-1 context
		hasher h;
	};

	struct cached_block_entry
	{
		cached_block_entry()
			: refcount(0)
			, dirty(0)
			, pending(0)
			, cache_hit(0)
		{
		}

		char* buf = nullptr;

		static constexpr int max_refcount = (1 << 29) - 1;

		// the number of references to this buffer. These references
		// might be in outstanding asynchronous requests or in peer
		// connection send buffers. We can't free the buffer until
		// all references are gone and refcount reaches 0. The buf
		// pointer in this struct doesn't count as a reference and
		// is always the last to be cleared
		std::uint32_t refcount:29;

		// if this is true, this block needs to be written to
		// disk before it's freed. Typically all blocks in a piece
		// would either be dirty (write coalesce cache) or not dirty
		// (read-ahead cache). Once blocks are written to disk, the
		// dirty flag is cleared and effectively turns the block
		// into a read cache block
		std::uint32_t dirty:1;

		// pending means that this buffer has not yet been filled in
		// with valid data. There's an outstanding read job for this.
		// If the dirty flag is set, it means there's an outstanding
		// write job to write this block.
		std::uint32_t pending:1;

		// this is set to 1 if this block has been read at least once. If the same
		// block is read twice, the whole piece is considered *frequently* used,
		// not just recently used.
		std::uint32_t cache_hit:1;

#if TORRENT_USE_ASSERTS
		// this many of the references are held by hashing operations
		int hashing_count = 0;
		// this block is being used in this many peer's send buffers currently
		int reading_count = 0;
		// the number of references held by flushing operations
		int flushing_count = 0;
#endif
	};

	// list_node is here to be able to link this cache entry
	// into one of the LRU lists
	struct TORRENT_EXTRA_EXPORT cached_piece_entry
		: list_node<cached_piece_entry>
		, boost::intrusive::list_base_hook<boost::intrusive::link_mode<
			boost::intrusive::auto_unlink>>
	{
		cached_piece_entry();
		~cached_piece_entry();
		cached_piece_entry(cached_piece_entry&&) = default;
		cached_piece_entry& operator=(cached_piece_entry&&) = default;

		bool ok_to_evict(bool ignore_hash = false) const
		{
			return refcount == 0
				&& piece_refcount == 0
				&& !hashing
				&& read_jobs.empty()
				&& outstanding_read == 0
				&& (ignore_hash || !hash || hash->offset == 0);
		}

		// storage this piece belongs to
		std::shared_ptr<storage_interface> storage;

		// write jobs hanging off of this piece
		tailqueue<disk_io_job> jobs;

		// read jobs waiting for the read job currently outstanding
		// on this piece to complete. These are executed at that point.
		tailqueue<disk_io_job> read_jobs;

		piece_index_t get_piece() const { return piece; }
		void* get_storage() const { return storage.get(); }

		bool operator==(cached_piece_entry const& rhs) const
		{ return piece == rhs.piece && storage.get() == rhs.storage.get(); }

		// if this is set, we'll be calculating the hash
		// for this piece. This member stores the interim
		// state while we're calculating the hash.
		std::unique_ptr<partial_hash> hash;

		// the pointers to the block data. If this is a ghost
		// cache entry, there won't be any data here
		aux::unique_ptr<cached_block_entry[]> blocks;

		// the last time a block was written to this piece
		// plus the minimum amount of time the block is guaranteed
		// to stay in the cache
		//TODO: make this 32 bits and to count seconds since the block cache was created
		time_point expire = min_time();

		piece_index_t piece{0};

		// the number of dirty blocks in this piece
		std::uint64_t num_dirty:14;

		// the number of blocks in the cache for this piece
		std::uint64_t num_blocks:14;

		// the total number of blocks in this piece (and the number
		// of elements in the blocks array)
		std::uint64_t blocks_in_piece:14;

		// ---- 64 bit boundary ----

		// while we have an outstanding async hash operation
		// working on this piece, 'hashing' is set to 1
		// When the operation returns, this is set to 0.
		std::uint16_t hashing:1;

		// if we've completed at least one hash job on this
		// piece, and returned it. This is set to one
		std::uint16_t hashing_done:1;

		// if this is true, whenever refcount hits 0,
		// this piece should be deleted from the cache
		// (not just demoted)
		std::uint16_t marked_for_deletion:1;

		// this is set to true once we flush blocks past
		// the hash cursor. Once this happens, there's
		// no point in keeping cache blocks around for
		// it in avoid_readback mode
		std::uint16_t need_readback:1;

		// indicates which LRU list this piece is chained into
		enum cache_state_t
		{
			// not added to the cache
			none,

			// this is the LRU list for pieces with dirty blocks
			write_lru,

			// this is the LRU list for volatile pieces. i.e.
			// pieces with very low cache priority. These are
			// always the first ones to be evicted.
			volatile_read_lru,

			// this is the LRU list for read blocks that have
			// been requested once
			read_lru1,

			// the is the LRU list for read blocks that have
			// been requested once recently, but then was evicted.
			// if these are requested again, they will be moved
			// to list 2, the frequently requested pieces
			read_lru1_ghost,

			// this is the LRU of frequently used pieces. Any
			// piece that has been requested by a second peer
			// while pulled in to list 1 by a different peer
			// is moved into this list
			read_lru2,

			// this is the LRU of frequently used pieces but
			// that has been recently evicted. If a piece in
			// this list is requested, it's moved back into list 2.
			read_lru2_ghost,
			num_lrus
		};

		std::uint16_t cache_state:3;

		// this is the number of threads that are currently holding
		// a reference to this piece. A piece may not be removed from
		// the cache while this is > 0
		std::uint16_t piece_refcount:7;

		// if this is set to one, it means there is an outstanding
		// flush_hashed job for this piece, and there's no need to
		// issue another one.
		std::uint16_t outstanding_flush:1;

		// as long as there is a read operation outstanding on this
		// piece, this is set to 1. Otherwise 0.
		// the purpose is to make sure not two threads are reading
		// the same blocks at the same time. If a new read job is
		// added when this is 1, that new job should be hung on the
		// read job queue (read_jobs).
		std::uint16_t outstanding_read:1;

		// this is set when the piece should be evicted as soon as there
		// no longer are any references to it. Evicted here means demoted
		// to a ghost list
		std::uint32_t marked_for_eviction:1;

		// the number of blocks that have >= 1 refcount
		std::uint32_t pinned:15;

		// ---- 32 bit boundary ---

		// the sum of all refcounts in all blocks
		std::int32_t refcount = 0;

#if TORRENT_USE_ASSERTS
		// the number of times this piece has finished hashing
		int hash_passes = 0;

		// this is a debug facility to keep a log
		// of which operations have been run on this piece
		aux::vector<piece_log_t> piece_log;

		bool in_storage = false;
		bool in_use = true;
#endif
	};

	struct TORRENT_EXTRA_EXPORT block_cache : disk_buffer_pool
	{
		block_cache(io_service& ios, std::function<void()> const& trigger_trim);
		~block_cache();

	private:

		struct hash_value
		{
			std::size_t operator()(cached_piece_entry const& p) const
			{ return reinterpret_cast<std::size_t>(p.storage.get()) + std::size_t(static_cast<int>(p.piece)); }
		};
		using cache_t = std::unordered_set<cached_piece_entry, hash_value>;

	public:

		using const_iterator = cache_t::const_iterator;

		// returns the number of blocks this job would cause to be read in
		int pad_job(disk_io_job const* j, int blocks_in_piece
			, int read_ahead) const;

		void reclaim_block(storage_interface* st, aux::block_cache_reference const& ref);

		// returns a range of all pieces. This might be a very
		// long list, use carefully
		std::pair<const_iterator, const_iterator> all_pieces() const;
		int num_pieces() const { return int(m_pieces.size()); }

		list_iterator<cached_piece_entry> write_lru_pieces() const
		{ return m_lru[cached_piece_entry::write_lru].iterate(); }

		int num_write_lru_pieces() const { return m_lru[cached_piece_entry::write_lru].size(); }

		enum eviction_mode
		{
			allow_ghost,
			disallow_ghost
		};

		// mark this piece for deletion. If there are no outstanding
		// requests to this piece, it's removed immediately, and the
		// passed in iterator will be invalidated
		void mark_for_eviction(cached_piece_entry* p, eviction_mode mode);

		// similar to mark_for_eviction, except for actually marking the
		// piece for deletion. If the piece was actually deleted,
		// the function returns true
		bool evict_piece(cached_piece_entry* p, tailqueue<disk_io_job>& jobs
			, eviction_mode mode);

		// if this piece is in L1 or L2 proper, move it to
		// its respective ghost list
		void move_to_ghost(cached_piece_entry* p);

		// returns the number of bytes read on success (cache hit)
		// -1 on cache miss
		int try_read(disk_io_job* j, buffer_allocator_interface& allocator
			, bool expect_no_fail = false);

		// called when we're reading and we found the piece we're
		// reading from in the hash table (not necessarily that we
		// hit the block we needed)
		void cache_hit(cached_piece_entry* p, int block, bool volatile_read);

		// free block from piece entry
		void free_block(cached_piece_entry* pe, int block);

		// erase a piece (typically from the ghost list). Reclaim all
		// its blocks and unlink it and free it.
		void erase_piece(cached_piece_entry* p);

		// bump the piece 'p' to the back of the LRU list it's
		// in (back == MRU)
		// this is only used for the write cache
		void bump_lru(cached_piece_entry* p);

		// move p into the correct lru queue
		void update_cache_state(cached_piece_entry* p);

		// if the piece is marked for deletion and has a refcount
		// of 0, this function will post any sync jobs and
		// delete the piece from the cache
		bool maybe_free_piece(cached_piece_entry* p);

		// either returns the piece in the cache, or allocates
		// a new empty piece and returns it.
		// cache_state is one of cache_state_t enum
		cached_piece_entry* allocate_piece(disk_io_job const* j, std::uint16_t cache_state);

		// looks for this piece in the cache. If it's there, returns a pointer
		// to it, otherwise 0.
		cached_piece_entry* find_piece(disk_io_job const* j);
		cached_piece_entry* find_piece(storage_interface* st, piece_index_t piece);

		// clear free all buffers marked as dirty with
		// refcount of 0.
		void abort_dirty(cached_piece_entry* p);

		// used to convert dirty blocks into non-dirty ones
		// i.e. from being part of the write cache to being part
		// of the read cache. it's used when flushing blocks to disk
		// returns true if the piece entry was freed
		bool blocks_flushed(cached_piece_entry* pe, int const* flushed, int num_flushed);

		// adds a block to the cache, marks it as dirty and
		// associates the job with it. When the block is
		// flushed, the callback is posted
		cached_piece_entry* add_dirty_block(disk_io_job* j);

		enum { blocks_inc_refcount = 1 };
		void insert_blocks(cached_piece_entry* pe, int block, span<iovec_t const> iov
			, disk_io_job* j, int flags = 0);

#if TORRENT_USE_INVARIANT_CHECKS
		void check_invariant() const;
#endif

		// try to remove num number of read cache blocks from the cache
		// pick the least recently used ones first
		// return the number of blocks that was requested to be evicted
		// that couldn't be
		int try_evict_blocks(int num, cached_piece_entry* ignore = nullptr);

		// try to evict a single volatile piece, if there is one.
		void try_evict_one_volatile();

		// if there are any dirty blocks
		void clear(tailqueue<disk_io_job>& jobs);

		void update_stats_counters(counters& c) const;
#if TORRENT_ABI_VERSION == 1
		void get_stats(cache_status* ret) const;
#endif
		void set_settings(aux::session_settings const& sett);

		enum reason_t { ref_hashing = 0, ref_reading = 1, ref_flushing = 2 };
		bool inc_block_refcount(cached_piece_entry* pe, int block, int reason);
		void dec_block_refcount(cached_piece_entry* pe, int block, int reason);

		int pinned_blocks() const { return m_pinned_blocks; }
		int read_cache_size() const { return m_read_cache_size; }

	private:

		// returns number of bytes read on success, -1 on cache miss
		// (just because the piece is in the cache, doesn't mean all
		// the blocks are there)
		int copy_from_piece(cached_piece_entry* p, disk_io_job* j
			, buffer_allocator_interface& allocator, bool expect_no_fail = false);

		int drain_piece_bufs(cached_piece_entry& p, std::vector<char*>& buf);

		// block container
		cache_t m_pieces;

		// linked list of all elements in m_pieces, in usage order
		// the most recently used are in the tail. iterating from head
		// to tail gives the least recently used entries first
		// the read-list is for read blocks and the write-list is for
		// dirty blocks that needs flushing before being evicted
		// [0] = write-LRU
		// [1] = read-LRU1
		// [2] = read-LRU1-ghost
		// [3] = read-LRU2
		// [4] = read-LRU2-ghost
		linked_list<cached_piece_entry> m_lru[cached_piece_entry::num_lrus];

		// this is used to determine whether to evict blocks from
		// L1 or L2.
		enum cache_op_t
		{
			cache_miss,
			ghost_hit_lru1,
			ghost_hit_lru2
		};
		int m_last_cache_op;

		// the number of pieces to keep in the ARC ghost lists
		// this is determined by being a fraction of the cache size
		int m_ghost_size;

		// the is the max number of volatile read cache blocks are allowed in the
		// cache. Once this is reached, other volatile blocks will start to be
		// evicted.
		int m_max_volatile_blocks;

		// the number of blocks (buffers) allocated by volatile pieces.
		std::int32_t m_volatile_size;

		// the number of blocks in the cache
		// that are in the read cache
		std::int32_t m_read_cache_size;

		// the number of blocks in the cache
		// that are in the write cache
		std::int32_t m_write_cache_size;

		// the number of blocks that are currently sitting
		// in peer's send buffers. If two peers are sending
		// the same block, it counts as 2, even though there're
		// no buffer duplication
		std::int32_t m_send_buffer_blocks;

		// the number of blocks with a refcount > 0, i.e.
		// they may not be evicted
		int m_pinned_blocks;
	};

}

#endif // TORRENT_BLOCK_CACHE
