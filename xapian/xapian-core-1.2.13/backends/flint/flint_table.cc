/* flint_table.cc: Btree implementation
 *
 * Copyright 1999,2000,2001 BrightStation PLC
 * Copyright 2002 Ananova Ltd
 * Copyright 2002,2003,2004,2005,2006,2007,2008,2009,2010,2011 Olly Betts
 * Copyright 2008 Lemur Consulting Ltd
 * Copyright 2010 Richard Boulton
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include "flint_table.h"

#include <xapian/error.h>

#include "safeerrno.h"
#ifdef __WIN32__
# include "msvc_posix_wrapper.h"
#endif

#include "omassert.h"
#include "stringutils.h" // For STRINGIZE().

// Define to use "dangerous" mode - in this mode we write modified btree
// blocks back in place.  This is somewhat faster (especially once we're
// I/O bound) but the database can't be safely searched during indexing
// and if indexing is terminated uncleanly, the database may be corrupt.
//
// Despite the risks, it's useful for speeding up a full rebuild.
//
// FIXME: make this mode run-time selectable, and record that it is currently
// in use somewhere on disk, so readers can check and refuse to open the
// database.
//
// #define DANGEROUS

#include <sys/types.h>

// Trying to include the correct headers with the correct defines set to
// get pread() and pwrite() prototyped on every platform without breaking any
// other platform is a real can of worms.  So instead we probe for what
// prototypes (if any) are required in configure and put them into
// PREAD_PROTOTYPE and PWRITE_PROTOTYPE.
#if defined HAVE_PREAD && defined PREAD_PROTOTYPE
PREAD_PROTOTYPE
#endif
#if defined HAVE_PWRITE && defined PWRITE_PROTOTYPE
PWRITE_PROTOTYPE
#endif

#include <cstdio>    /* for rename */
#include <cstring>   /* for memmove */
#include <climits>   /* for CHAR_BIT */

#include "flint_btreebase.h"
#include "flint_cursor.h"
#include "flint_utils.h"

#include "debuglog.h"
#include "io_utils.h"
#include "omassert.h"
#include "str.h"
#include "unaligned.h"
#include "utils.h"

#include <algorithm>  // for std::min()
#include <string>

using namespace std;

// Only try to compress tags longer than this many bytes.
const size_t COMPRESS_MIN = 4;

//#define BTREE_DEBUG_FULL 1
#undef BTREE_DEBUG_FULL

#ifdef BTREE_DEBUG_FULL
/*------debugging aids from here--------*/

static void print_key(const Byte * p, int c, int j);
static void print_tag(const Byte * p, int c, int j);

/*
static void report_cursor(int N, Btree * B, Cursor_ * C)
{
    int i;
    printf("%d)\n", N);
    for (i = 0; i <= B->level; i++)
	printf("p=%d, c=%d, n=[%d], rewrite=%d\n",
		C[i].p, C[i].c, C[i].n, C[i].rewrite);
}
*/

/*------to here--------*/
#endif /* BTREE_DEBUG_FULL */

static inline Byte *zeroed_new(size_t size)
{
    Byte *temp = new Byte[size];
    // No need to check if temp is NULL, new throws std::bad_alloc if
    // allocation fails.
    Assert(temp);
    memset(temp, 0, size);
    return temp;
}

/* A B-tree comprises (a) a base file, containing essential information (Block
   size, number of the B-tree root block etc), (b) a bitmap, the Nth bit of the
   bitmap being set if the Nth block of the B-tree file is in use, and (c) a
   file DB containing the B-tree proper. The DB file is divided into a sequence
   of equal sized blocks, numbered 0, 1, 2 ... some of which are free, some in
   use. Those in use are arranged in a tree.

   Each block, b, has a structure like this:

     R L M T D o1 o2 o3 ... oN <gap> [item] .. [item] .. [item] ...
     <---------- D ----------> <-M->

   And then,

   R = REVISION(b)  is the revision number the B-tree had when the block was
                    written into the DB file.
   L = GET_LEVEL(b) is the level of the block, which is the number of levels
                    towards the root of the B-tree structure. So leaf blocks
                    have level 0 and the one root block has the highest level
                    equal to the number of levels in the B-tree.
   M = MAX_FREE(b)  is the size of the gap between the end of the directory and
                    the first item of data. (It is not necessarily the maximum
                    size among the bits of space that are free, but I can't
                    think of a better name.)
   T = TOTAL_FREE(b)is the total amount of free space left in b.
   D = DIR_END(b)   gives the offset to the end of the directory.

   o1, o2 ... oN are a directory of offsets to the N items held in the block.
   The items are key-tag pairs, and as they occur in the directory are ordered
   by the keys.

   An item has this form:

           I K key x C tag
             <--K-->
           <------I------>

   A long tag presented through the API is split up into C tags small enough to
   be accommodated in the blocks of the B-tree. The key is extended to include
   a counter, x, which runs from 1 to C. The key is preceded by a length, K,
   and the whole item with a length, I, as depicted above.

   Here are the corresponding definitions:

*/

#define REVISION(b)      static_cast<unsigned int>(getint4(b, 0))
#define GET_LEVEL(b)     getint1(b, 4)
#define MAX_FREE(b)      getint2(b, 5)
#define TOTAL_FREE(b)    getint2(b, 7)
#define DIR_END(b)       getint2(b, 9)
#define DIR_START        11

#define SET_REVISION(b, x)      setint4(b, 0, x)
#define SET_LEVEL(b, x)         setint1(b, 4, x)
#define SET_MAX_FREE(b, x)      setint2(b, 5, x)
#define SET_TOTAL_FREE(b, x)    setint2(b, 7, x)
#define SET_DIR_END(b, x)       setint2(b, 9, x)

/** Flip to sequential addition block-splitting after this number of observed
 *  sequential additions (in negated form). */
#define SEQ_START_POINT (-10)



/* There are two bit maps in bit_map0 and bit_map. The nth bit of bitmap is 0
   if the nth block is free, otherwise 1. bit_map0 is the initial state of
   bitmap at the start of the current transaction.

   Note use of the limits.h values:
   UCHAR_MAX = 255, an unsigned with all bits set, and
   CHAR_BIT = 8, the number of bits per byte

   BYTE_PAIR_RANGE below is the smallest +ve number that can't be held in two
   bytes -- 64K effectively.
*/

#define BYTE_PAIR_RANGE (1 << 2 * CHAR_BIT)

/// read_block(n, p) reads block n of the DB file to address p.
void
FlintTable::read_block(uint4 n, Byte * p) const
{
    // Log the value of p, not the contents of the block it points to...
    LOGCALL_VOID(DB, "FlintTable::read_block", n | (void*)p);
    /* Use the base bit_map_size not the bitmap's size, because
     * the latter is uninitialised in readonly mode.
     */
    Assert(n / CHAR_BIT < base.get_bit_map_size());

#ifdef HAVE_PREAD
    off_t offset = off_t(block_size) * n;
    int m = block_size;
    while (true) {
	ssize_t bytes_read = pread(handle, reinterpret_cast<char *>(p), m,
				   offset);
	// normal case - read succeeded, so return.
	if (bytes_read == m) return;
	if (bytes_read == -1) {
	    if (errno == EINTR) continue;
	    string message = "Error reading block " + str(n) + ": ";
	    message += strerror(errno);
	    throw Xapian::DatabaseError(message);
	} else if (bytes_read == 0) {
	    string message = "Error reading block " + str(n) + ": got end of file";
	    throw Xapian::DatabaseError(message);
	} else if (bytes_read < m) {
	    /* Read part of the block, which is not an error.  We should
	     * continue reading the rest of the block.
	     */
	    m -= int(bytes_read);
	    p += bytes_read;
	    offset += bytes_read;
	}
    }
#else
    if (lseek(handle, off_t(block_size) * n, SEEK_SET) == -1) {
	string message = "Error seeking to block: ";
	message += strerror(errno);
	throw Xapian::DatabaseError(message);
    }

    io_read(handle, reinterpret_cast<char *>(p), block_size, block_size);
#endif
}

/** write_block(n, p) writes block n in the DB file from address p.
 *  When writing we check to see if the DB file has already been
 *  modified. If not (so this is the first write) the old base is
 *  deleted. This prevents the possibility of it being opened
 *  subsequently as an invalid base.
 */
void
FlintTable::write_block(uint4 n, const Byte * p) const
{
    LOGCALL_VOID(DB, "FlintTable::write_block", n | p);
    Assert(writable);
    /* Check that n is in range. */
    Assert(n / CHAR_BIT < base.get_bit_map_size());

    /* don't write to non-free */;
    AssertParanoid(base.block_free_at_start(n));

    /* write revision is okay */
    AssertEqParanoid(REVISION(p), latest_revision_number + 1);

    if (both_bases) {
	// Delete the old base before modifying the database.
	//
	// If the file is on NFS, then io_unlink() may return false even if
	// the file was removed, so on balance throwing an exception in this
	// case is unhelpful, since we wanted the file gone anyway!  The
	// likely explanation is that somebody moved, deleted, or changed a
	// symlink to the database directory.
	(void)io_unlink(name + "base" + other_base_letter());
	both_bases = false;
	latest_revision_number = revision_number;
    }

#ifdef HAVE_PWRITE
    off_t offset = off_t(block_size) * n;
    int m = block_size;
    while (true) {
	ssize_t bytes_written = pwrite(handle, p, m, offset);
	if (bytes_written == m) {
	    // normal case - write succeeded, so return.
	    return;
	} else if (bytes_written == -1) {
	    if (errno == EINTR) continue;
	    string message = "Error writing block: ";
	    message += strerror(errno);
	    throw Xapian::DatabaseError(message);
	} else if (bytes_written == 0) {
	    string message = "Error writing block: wrote no data";
	    throw Xapian::DatabaseError(message);
	} else if (bytes_written < m) {
	    /* Wrote part of the block, which is not an error.  We should
	     * continue writing the rest of the block.
	     */
	    m -= int(bytes_written);
	    p += bytes_written;
	    offset += bytes_written;
	}
    }
#else
    if (lseek(handle, (off_t)block_size * n, SEEK_SET) == -1) {
	string message = "Error seeking to block: ";
	message += strerror(errno);
	throw Xapian::DatabaseError(message);
    }

    io_write(handle, reinterpret_cast<const char *>(p), block_size);
#endif
}


/* A note on cursors:

   Each B-tree level has a corresponding array element C[j] in a
   cursor, C. C[0] is the leaf (or data) level, and C[B->level] is the
   root block level. Within a level j,

       C[j].p  addresses the block
       C[j].c  is the offset into the directory entry in the block
       C[j].n  is the number of the block at C[j].p

   A look up in the B-tree causes navigation of the blocks starting
   from the root. In each block, p, we find an offset, c, to an item
   which gives the number, n, of the block for the next level. This
   leads to an array of values p,c,n which are held inside the cursor.

   Any Btree object B has a built-in cursor, at B->C. But other cursors may
   be created.  If BC is a created cursor, BC->C is the cursor in the
   sense given above, and BC->B is the handle for the B-tree again.
*/


void
FlintTable::set_overwritten() const
{
    LOGCALL_VOID(DB, "FlintTable::set_overwritten", NO_ARGS);
    // If we're writable, there shouldn't be another writer who could cause
    // overwritten to be flagged, so that's a DatabaseCorruptError.
    if (writable)
	throw Xapian::DatabaseCorruptError("Db block overwritten - are there multiple writers?");
    throw Xapian::DatabaseModifiedError("The revision being read has been discarded - you should call Xapian::Database::reopen() and retry the operation");
}

/* block_to_cursor(C, j, n) puts block n into position C[j] of cursor
   C, writing the block currently at C[j] back to disk if necessary.
   Note that

       C[j].rewrite

   is true iff C[j].n is different from block n in file DB. If it is
   false no rewriting is necessary.
*/

void
FlintTable::block_to_cursor(Cursor_ * C_, int j, uint4 n) const
{
    LOGCALL_VOID(DB, "FlintTable::block_to_cursor", (void*)C_ | j | n);
    if (n == C_[j].n) return;
    Byte * p = C_[j].p;
    Assert(p);

    // FIXME: only needs to be done in write mode
    if (C_[j].rewrite) {
	Assert(writable);
	Assert(C == C_);
	write_block(C_[j].n, p);
	C_[j].rewrite = false;
    }
    // Check if the block is in the built-in cursor (potentially in
    // modified form).
    if (writable && n == C[j].n) {
	if (p != C[j].p)
	    memcpy(p, C[j].p, block_size);
    } else {
	read_block(n, p);
    }

    C_[j].n = n;
    if (j < level) {
	/* unsigned comparison */
	if (REVISION(p) > REVISION(C_[j + 1].p)) {
	    set_overwritten();
	    return;
	}
    }
    AssertEq(j, GET_LEVEL(p));
}

/** Btree::alter(); is called when the B-tree is to be altered.

   It causes new blocks to be forced for the current set of blocks in
   the cursor.

   The point is that if a block at level 0 is to be altered it may get
   a new number. Then the pointer to this block from level 1 will need
   changing. So the block at level 1 needs altering and may get a new
   block number. Then the pointer to this block from level 2 will need
   changing ... and so on back to the root.

   The clever bit here is spotting the cases when we can make an early
   exit from this process. If C[j].rewrite is true, C[j+k].rewrite
   will be true for k = 1,2 ... We have been through all this before,
   and there is no need to do it again. If C[j].n was free at the
   start of the transaction, we can copy it back to the same place
   without violating the integrity of the B-tree. We don't then need a
   new n and can return. The corresponding C[j].rewrite may be true or
   false in that case.
*/

void
FlintTable::alter()
{
    LOGCALL_VOID(DB, "FlintTable::alter", NO_ARGS);
    Assert(writable);
#ifdef DANGEROUS
    C[0].rewrite = true;
#else
    int j = 0;
    Byte * p = C[j].p;
    while (true) {
	if (C[j].rewrite) return; /* all new, so return */
	C[j].rewrite = true;

	uint4 n = C[j].n;
	if (base.block_free_at_start(n)) {
	    Assert(REVISION(p) == latest_revision_number + 1);
	    return;
	}
	Assert(REVISION(p) < latest_revision_number + 1);
	base.free_block(n);
	n = base.next_free_block();
	C[j].n = n;
	SET_REVISION(p, latest_revision_number + 1);

	if (j == level) return;
	j++;
	p = C[j].p;
	Item_wr_(p, C[j].c).set_block_given_by(n);
    }
#endif
}

/** find_in_block(p, key, leaf, c) searches for the key in the block at p.

   leaf is true for a data block, and false for an index block (when the
   first key is dummy and never needs to be tested). What we get is the
   directory entry to the last key <= the key being searched for.

   The lookup is by binary chop, with i and j set to the left and
   right ends of the search area. In sequential addition, c will often
   be the answer, so we test the keys round c and move i and j towards
   c if possible.
*/

int FlintTable::find_in_block(const Byte * p, Key_ key, bool leaf, int c)
{
    LOGCALL_STATIC(DB, int, "FlintTable::find_in_block", reinterpret_cast<const void*>(p) | reinterpret_cast<const void*>(key.get_address()) | leaf | c);
    int i = DIR_START;
    if (leaf) i -= D2;
    int j = DIR_END(p);

    if (c != -1) {
	if (c < j && i < c && Item_(p, c).key() <= key)
	    i = c;
	c += D2;
	if (c < j && i < c && key < Item_(p, c).key())
	    j = c;
    }

    while (j - i > D2) {
	int k = i + ((j - i)/(D2 * 2))*D2; /* mid way */
	if (key < Item_(p, k).key()) j = k; else i = k;
    }
    RETURN(i);
}

/** find(C_) searches for the key of B->kt in the B-tree.

   Result is true if found, false otherwise.  When false, the B_tree
   cursor is positioned at the last key in the B-tree <= the search
   key.  Goes to first (null) item in B-tree when key length == 0.
*/

bool
FlintTable::find(Cursor_ * C_) const
{
    LOGCALL(DB, bool, "FlintTable::find", (void*)C_);
    // Note: the parameter is needed when we're called by FlintCursor
    const Byte * p;
    int c;
    Key_ key = kt.key();
    for (int j = level; j > 0; --j) {
	p = C_[j].p;
	c = find_in_block(p, key, false, C_[j].c);
#ifdef BTREE_DEBUG_FULL
	printf("Block in FlintTable:find - code position 1");
	report_block_full(j, C_[j].n, p);
#endif /* BTREE_DEBUG_FULL */
	C_[j].c = c;
	block_to_cursor(C_, j - 1, Item_(p, c).block_given_by());
    }
    p = C_[0].p;
    c = find_in_block(p, key, true, C_[0].c);
#ifdef BTREE_DEBUG_FULL
    printf("Block in FlintTable:find - code position 2");
    report_block_full(0, C_[0].n, p);
#endif /* BTREE_DEBUG_FULL */
    C_[0].c = c;
    if (c < DIR_START) RETURN(false);
    RETURN(Item_(p, c).key() == key);
}

/** compact(p) compact the block at p by shuffling all the items up to the end.

   MAX_FREE(p) is then maximized, and is equal to TOTAL_FREE(p).
*/

void
FlintTable::compact(Byte * p)
{
    LOGCALL_VOID(DB, "FlintTable::compact", (void*)p);
    Assert(writable);
    int e = block_size;
    Byte * b = buffer;
    int dir_end = DIR_END(p);
    for (int c = DIR_START; c < dir_end; c += D2) {
	Item_ item(p, c);
	int l = item.size();
	e -= l;
	memmove(b + e, item.get_address(), l);
	setD(p, c, e);  /* reform in b */
    }
    memmove(p + e, b + e, block_size - e);  /* copy back */
    e -= dir_end;
    SET_TOTAL_FREE(p, e);
    SET_MAX_FREE(p, e);
}

/** Btree needs to gain a new level to insert more items: so split root block
 *  and construct a new one.
 */
void
FlintTable::split_root(uint4 split_n)
{
    LOGCALL_VOID(DB, "FlintTable::split_root", split_n);
    /* gain a level */
    ++level;

    /* check level overflow - this isn't something that should ever happen
     * but deserves more than an Assert()... */
    if (level == BTREE_CURSOR_LEVELS) {
	throw Xapian::DatabaseCorruptError("Btree has grown impossibly large ("STRINGIZE(BTREE_CURSOR_LEVELS)" levels)");
    }

    Byte * q = zeroed_new(block_size);
    C[level].p = q;
    C[level].c = DIR_START;
    C[level].n = base.next_free_block();
    C[level].rewrite = true;
    SET_REVISION(q, latest_revision_number + 1);
    SET_LEVEL(q, level);
    SET_DIR_END(q, DIR_START);
    compact(q);   /* to reset TOTAL_FREE, MAX_FREE */

    /* form a null key in b with a pointer to the old root */
    Byte b[10]; /* 7 is exact */
    Item_wr_ item(b);
    item.form_null_key(split_n);
    add_item(item, level);
}

/** enter_key(j, prevkey, newkey) is called after a block split.

   It enters in the block at level C[j] a separating key for the block
   at level C[j - 1]. The key itself is newkey. prevkey is the
   preceding key, and at level 1 newkey can be trimmed down to the
   first point of difference to prevkey for entry in C[j].

   This code looks longer than it really is. If j exceeds the number
   of B-tree levels the root block has split and we have to construct
   a new one, but this is a rare event.

   The key is constructed in b, with block number C[j - 1].n as tag,
   and this is added in with add_item. add_item may itself cause a
   block split, with a further call to enter_key. Hence the recursion.
*/
void
FlintTable::enter_key(int j, Key_ prevkey, Key_ newkey)
{
    Assert(writable);
    Assert(prevkey < newkey);
    Assert(j >= 1);

    uint4 blocknumber = C[j - 1].n;

    // FIXME update to use Key_
    // Keys are truncated here: but don't truncate the count at the end away.
    const int newkey_len = newkey.length();
    int i;

    if (j == 1) {
	// Truncate the key to the minimal key which differs from prevkey,
	// the preceding key in the block.
	i = 0;
	const int min_len = min(newkey_len, prevkey.length());
	while (i < min_len && prevkey[i] == newkey[i]) {
	    i++;
	}

	// Want one byte of difference.
	if (i < newkey_len) i++;
    } else {
	/* Can't truncate between branch levels, since the separated keys
	 * are in at the leaf level, and truncating again will change the
	 * branch point.
	 */
	i = newkey_len;
    }

    Byte b[UCHAR_MAX + 6];
    Item_wr_ item(b);
    Assert(i <= 256 - I2 - C2);
    Assert(i <= (int)sizeof(b) - I2 - C2 - 4);
    item.set_key_and_block(newkey, i, blocknumber);

    // When j > 1 we can make the first key of block p null.  This is probably
    // worthwhile as it trades a small amount of CPU and RAM use for a small
    // saving in disk use.  Other redundant keys will still creep in though.
    if (j > 1) {
	Byte * p = C[j - 1].p;
	uint4 n = getint4(newkey.get_address(), newkey_len + K1 + C2);
	int new_total_free = TOTAL_FREE(p) + newkey_len + C2;
	// FIXME: incredibly icky going from key to item like this...
	Item_wr_(const_cast<Byte*>(newkey.get_address()) - I2).form_null_key(n);
	SET_TOTAL_FREE(p, new_total_free);
    }

    C[j].c = find_in_block(C[j].p, item.key(), false, 0) + D2;
    C[j].rewrite = true; /* a subtle point: this *is* required. */
    add_item(item, j);
}

/** mid_point(p) finds the directory entry in c that determines the
   approximate mid point of the data in the block at p.
 */

int
FlintTable::mid_point(Byte * p)
{
    int n = 0;
    int dir_end = DIR_END(p);
    int size = block_size - TOTAL_FREE(p) - dir_end;
    for (int c = DIR_START; c < dir_end; c += D2) {
	int l = Item_(p, c).size();
	n += 2 * l;
	if (n >= size) {
	    if (l < n - size) return c;
	    return c + D2;
	}
    }

    /* falling out of mid_point */
    Assert(false);
    return 0; /* Stop compiler complaining about end of method. */
}

/** add_item_to_block(p, kt_, c) adds item kt_ to the block at p.

   c is the offset in the directory that needs to be expanded to
   accommodate the new entry for the item. We know before this is
   called that there is enough room, so it's just a matter of byte
   shuffling.
*/

void
FlintTable::add_item_to_block(Byte * p, Item_wr_ kt_, int c)
{
    Assert(writable);
    int dir_end = DIR_END(p);
    int kt_len = kt_.size();
    int needed = kt_len + D2;
    int new_total = TOTAL_FREE(p) - needed;
    int new_max = MAX_FREE(p) - needed;

    Assert(new_total >= 0);

    if (new_max < 0) {
	compact(p);
	new_max = MAX_FREE(p) - needed;
	Assert(new_max >= 0);
    }
    Assert(dir_end >= c);

    memmove(p + c + D2, p + c, dir_end - c);
    dir_end += D2;
    SET_DIR_END(p, dir_end);

    int o = dir_end + new_max;
    setD(p, c, o);
    memmove(p + o, kt_.get_address(), kt_len);

    SET_MAX_FREE(p, new_max);

    SET_TOTAL_FREE(p, new_total);
}

/** FlintTable::add_item(kt_, j) adds item kt_ to the block at cursor level C[j].
 *
 *  If there is not enough room the block splits and the item is then
 *  added to the appropriate half.
 */
void
FlintTable::add_item(Item_wr_ kt_, int j)
{
    Assert(writable);
    Byte * p = C[j].p;
    int c = C[j].c;
    uint4 n;

    int needed = kt_.size() + D2;
    if (TOTAL_FREE(p) < needed) {
	int m;
	// Prepare to split p. After splitting, the block is in two halves, the
	// lower half is split_p, the upper half p again. add_to_upper_half
	// becomes true when the item gets added to p, false when it gets added
	// to split_p.

	if (seq_count < 0) {
	    // If we're not in sequential mode, we split at the mid point
	    // of the node.
	    m = mid_point(p);
	} else {
	    // During sequential addition, split at the insert point
	    m = c;
	}

	uint4 split_n = C[j].n;
	C[j].n = base.next_free_block();

	memcpy(split_p, p, block_size);  // replicate the whole block in split_p
	SET_DIR_END(split_p, m);
	compact(split_p);      /* to reset TOTAL_FREE, MAX_FREE */

	{
	    int residue = DIR_END(p) - m;
	    int new_dir_end = DIR_START + residue;
	    memmove(p + DIR_START, p + m, residue);
	    SET_DIR_END(p, new_dir_end);
	}

	compact(p);      /* to reset TOTAL_FREE, MAX_FREE */

	bool add_to_upper_half;
	if (seq_count < 0) {
	    add_to_upper_half = (c >= m);
	} else {
	    // And add item to lower half if split_p has room, otherwise upper
	    // half
	    add_to_upper_half = (TOTAL_FREE(split_p) < needed);
	}

	if (add_to_upper_half) {
	    c -= (m - DIR_START);
	    Assert(seq_count < 0 || c <= DIR_START + D2);
	    Assert(c >= DIR_START);
	    Assert(c <= DIR_END(p));
	    add_item_to_block(p, kt_, c);
	    n = C[j].n;
	} else {
	    Assert(c >= DIR_START);
	    Assert(c <= DIR_END(split_p));
	    add_item_to_block(split_p, kt_, c);
	    n = split_n;
	}
	write_block(split_n, split_p);

	// Check if we're splitting the root block.
	if (j == level) split_root(split_n);

	/* Enter a separating key at level j + 1 between */
	/* the last key of block split_p, and the first key of block p */
	enter_key(j + 1,
		  Item_(split_p, DIR_END(split_p) - D2).key(),
		  Item_(p, DIR_START).key());
    } else {
	add_item_to_block(p, kt_, c);
	n = C[j].n;
    }
    if (j == 0) {
	changed_n = n;
	changed_c = c;
    }
}

/** FlintTable::delete_item(j, repeatedly) is (almost) the converse of add_item.
 *
 * If repeatedly is true, the process repeats at the next level when a
 * block has been completely emptied, freeing the block and taking out
 * the pointer to it.  Emptied root blocks are also removed, which
 * reduces the number of levels in the B-tree.
 */
void
FlintTable::delete_item(int j, bool repeatedly)
{
    Assert(writable);
    Byte * p = C[j].p;
    int c = C[j].c;
    int kt_len = Item_(p, c).size(); /* size of the item to be deleted */
    int dir_end = DIR_END(p) - D2;   /* directory length will go down by 2 bytes */

    memmove(p + c, p + c + D2, dir_end - c);
    SET_DIR_END(p, dir_end);
    SET_MAX_FREE(p, MAX_FREE(p) + D2);
    SET_TOTAL_FREE(p, TOTAL_FREE(p) + kt_len + D2);

    if (!repeatedly) return;
    if (j < level) {
	if (dir_end == DIR_START) {
	    base.free_block(C[j].n);
	    C[j].rewrite = false;
	    C[j].n = BLK_UNUSED;
	    C[j + 1].rewrite = true;  /* *is* necessary */
	    delete_item(j + 1, true);
	}
    } else {
	Assert(j == level);
	while (dir_end == DIR_START + D2 && level > 0) {
	    /* single item in the root block, so lose a level */
	    uint4 new_root = Item_(p, DIR_START).block_given_by();
	    delete [] p;
	    C[level].p = 0;
	    base.free_block(C[level].n);
	    C[level].rewrite = false;
	    C[level].n = BLK_UNUSED;
	    level--;

	    block_to_cursor(C, level, new_root);

	    p = C[level].p;
	    dir_end = DIR_END(p); /* prepare for the loop */
	}
    }
}

/* debugging aid:
static addcount = 0;
*/

/** add_kt(found) adds the item (key-tag pair) at B->kt into the
   B-tree, using cursor C.

   found == find() is handed over as a parameter from Btree::add.
   Btree::alter() prepares for the alteration to the B-tree. Then
   there are a number of cases to consider:

     If an item with the same key is in the B-tree (found is true),
     the new kt replaces it.

     If then kt is smaller, or the same size as, the item it replaces,
     kt is put in the same place as the item it replaces, and the
     TOTAL_FREE measure is reduced.

     If kt is larger than the item it replaces it is put in the
     MAX_FREE space if there is room, and the directory entry and
     space counts are adjusted accordingly.

     - But if there is not room we do it the long way: the old item is
     deleted with delete_item and kt is added in with add_item.

     If the key of kt is not in the B-tree (found is false), the new
     kt is added in with add_item.
*/

int
FlintTable::add_kt(bool found)
{
    Assert(writable);
    int components = 0;

    /*
    {
	printf("%d) %s ", addcount++, (found ? "replacing" : "adding"));
	print_bytes(kt[I2] - K1 - C2, kt + I2 + K1); putchar('\n');
    }
    */
    alter();

    if (found) { /* replacement */
	seq_count = SEQ_START_POINT;
	sequential = false;

	Byte * p = C[0].p;
	int c = C[0].c;
	Item_ item(p, c);
	int kt_size = kt.size();
	int needed = kt_size - item.size();

	components = Item_(p, c).components_of();

	if (needed <= 0) {
	    /* simple replacement */
	    memmove(const_cast<Byte *>(item.get_address()),
		    kt.get_address(), kt_size);
	    SET_TOTAL_FREE(p, TOTAL_FREE(p) - needed);
	} else {
	    /* new item into the block's freespace */
	    int new_max = MAX_FREE(p) - kt_size;
	    if (new_max >= 0) {
		int o = DIR_END(p) + new_max;
		memmove(p + o, kt.get_address(), kt_size);
		setD(p, c, o);
		SET_MAX_FREE(p, new_max);
		SET_TOTAL_FREE(p, TOTAL_FREE(p) - needed);
	    } else {
		/* do it the long way */
		delete_item(0, false);
		add_item(kt, 0);
	    }
	}
    } else {
	/* addition */
	if (changed_n == C[0].n && changed_c == C[0].c) {
	    if (seq_count < 0) seq_count++;
	} else {
	    seq_count = SEQ_START_POINT;
	    sequential = false;
	}
	C[0].c += D2;
	add_item(kt, 0);
    }
    return components;
}

/* delete_kt() corresponds to add_kt(found), but there are only
   two cases: if the key is not found nothing is done, and if it is
   found the corresponding item is deleted with delete_item.
*/

int
FlintTable::delete_kt()
{
    Assert(writable);

    bool found = find(C);

    int components = 0;
    seq_count = SEQ_START_POINT;
    sequential = false;

    /*
    {
	printf("%d) %s ", addcount++, (found ? "deleting " : "ignoring "));
	print_bytes(B->kt[I2] - K1 - C2, B->kt + I2 + K1); putchar('\n');
    }
    */
    if (found) {
	components = Item_(C[0].p, C[0].c).components_of();
	alter();
	delete_item(0, true);
    }
    return components;
}

/* FlintTable::form_key(key) treats address kt as an item holder and fills in
the key part:

	   (I) K key c (C tag)

The bracketed parts are left blank. The key is filled in with key_len bytes and
K set accordingly. c is set to 1.
*/

void FlintTable::form_key(const string & key) const
{
    kt.form_key(key);
}

/* FlintTable::add(key, tag) adds the key/tag item to the
   B-tree, replacing any existing item with the same key.

   For a long tag, we end up having to add m components, of the form

       key 1 m tag1
       key 2 m tag2
       ...
       key m m tagm

   and tag1+tag2+...+tagm are equal to tag. These in their turn may be replacing
   n components of the form

       key 1 n TAG1
       key 2 n TAG2
       ...
       key n n TAGn

   and n may be greater than, equal to, or less than m. These cases are dealt
   with in the code below. If m < n for example, we end up with a series of
   deletions.
*/

void
FlintTable::add(const string &key, string tag, bool already_compressed)
{
    LOGCALL_VOID(DB, "FlintTable::add", key | tag);
    Assert(writable);

    if (handle < 0) create_and_open(block_size);

    form_key(key);

    bool compressed = false;
    if (already_compressed) {
	compressed = true;
    } else if (compress_strategy != DONT_COMPRESS && tag.size() > COMPRESS_MIN) {
	CompileTimeAssert(DONT_COMPRESS != Z_DEFAULT_STRATEGY);
	CompileTimeAssert(DONT_COMPRESS != Z_FILTERED);
	CompileTimeAssert(DONT_COMPRESS != Z_HUFFMAN_ONLY);
#ifdef Z_RLE
	CompileTimeAssert(DONT_COMPRESS != Z_RLE);
#endif

	lazy_alloc_deflate_zstream();

	// zlib takes a non-const pointer to the input, but doesn't modify it.
	char * non_const_tag = const_cast<char *>(tag.data());
	deflate_zstream->next_in = reinterpret_cast<Byte *>(non_const_tag);
	deflate_zstream->avail_in = uInt(tag.size());

	// If compressed size is >= tag.size(), we don't want to compress.
	unsigned long blk_len = tag.size() - 1;
	unsigned char * blk = new unsigned char[blk_len];
	deflate_zstream->next_out = blk;
	deflate_zstream->avail_out = uInt(blk_len);

	int err = deflate(deflate_zstream, Z_FINISH);
	if (err == Z_STREAM_END) {
	    // If deflate succeeded, then the output was at least one byte
	    // smaller than the input.
	    tag.assign(reinterpret_cast<const char *>(blk), deflate_zstream->total_out);
	    compressed = true;
	} else {
	    // Deflate failed - presumably the data wasn't compressible.
	}

	delete [] blk;
    }

    // sort of matching kt.append_chunk(), but setting the chunk
    const size_t cd = kt.key().length() + K1 + I2 + C2 + C2;  // offset to the tag data
    const size_t L = max_item_size - cd; // largest amount of tag data for any chunk
    size_t first_L = L;                  // - amount for tag1
    bool found = find(C);
    if (!found) {
	Byte * p = C[0].p;
	size_t n = TOTAL_FREE(p) % (max_item_size + D2);
	if (n > D2 + cd) {
	    n -= (D2 + cd);
	    // if n >= last then fully filling this block won't produce
	    // an extra item, so we might as well do this even if
	    // full_compaction isn't active.
	    //
	    // In the full_compaction case, it turns out we shouldn't always
	    // try to fill every last byte.  Doing so can actually increase the
	    // total space required (I believe this effect is due to longer
	    // dividing keys being required in the index blocks).  Empirically,
	    // n >= key.size() + K appears a good criterion for K ~= 34.  This
	    // seems to save about 0.2% in total database size over always
	    // splitting the tag.  It'll also give be slightly faster retrieval
	    // as we can avoid reading an extra block occasionally.
	    size_t last = tag.length() % L;
	    if (n >= last || (full_compaction && n >= key.size() + 34))
		first_L = n;
	}
    }

    // a null tag must be added in of course
    int m = tag.empty() ? 1 : (tag.length() - first_L + L - 1) / L + 1;
				      // there are m items to add
    /* FIXME: sort out this error higher up and turn this into
     * an assert.
     */
    if (m >= BYTE_PAIR_RANGE)
	throw Xapian::UnimplementedError("Can't handle insanely large tags");

    int n = 0; // initialise to shut off warning
				      // - and there will be n to delete
    int o = 0;                        // Offset into the tag
    size_t residue = tag.length();    // Bytes of the tag remaining to add in
    int replacement = false;          // Has there been a replacement ?
    int i;
    kt.set_components_of(m);
    for (i = 1; i <= m; i++) {
	size_t l = (i == m ? residue : (i == 1 ? first_L : L));
	Assert(cd + l <= block_size);
	Assert(string::size_type(o + l) <= tag.length());
	kt.set_tag(cd, tag.data() + o, l, compressed);
	kt.set_component_of(i);

	o += l;
	residue -= l;

	if (i > 1) found = find(C);
	n = add_kt(found);
	if (n > 0) replacement = true;
    }
    /* o == tag.length() here, and n may be zero */
    for (i = m + 1; i <= n; i++) {
	kt.set_component_of(i);
	delete_kt();
    }
    if (!replacement) ++item_count;
    Btree_modified = true;
    if (cursor_created_since_last_modification) {
	cursor_created_since_last_modification = false;
	++cursor_version;
    }
}

/* FlintTable::del(key) returns false if the key is not in the B-tree,
   otherwise deletes it and returns true.

   Again, this is parallel to FlintTable::add, but simpler in form.
*/

bool
FlintTable::del(const string &key)
{
    LOGCALL(DB, bool, "FlintTable::del", key);
    Assert(writable);

    if (handle < 0) {
	if (handle == -2) {
	    FlintTable::throw_database_closed();
	}
	RETURN(false);
    }

    // We can't delete a key which we is too long for us to store.
    if (key.size() > FLINT_BTREE_MAX_KEY_LEN) RETURN(false);

    if (key.empty()) RETURN(false);
    form_key(key);

    int n = delete_kt();  /* there are n items to delete */
    if (n <= 0) RETURN(false);

    for (int i = 2; i <= n; i++) {
	kt.set_component_of(i);
	delete_kt();
    }

    item_count--;
    Btree_modified = true;
    if (cursor_created_since_last_modification) {
	cursor_created_since_last_modification = false;
	++cursor_version;
    }
    RETURN(true);
}

bool
FlintTable::get_exact_entry(const string &key, string & tag) const
{
    LOGCALL(DB, bool, "FlintTable::get_exact_entry", key | tag);
    Assert(!key.empty());

    if (handle < 0) {
	if (handle == -2) {
	    FlintTable::throw_database_closed();
	}
	RETURN(false);
    }

    // An oversized key can't exist, so attempting to search for it should fail.
    if (key.size() > FLINT_BTREE_MAX_KEY_LEN) RETURN(false);

    form_key(key);
    if (!find(C)) RETURN(false);

    (void)read_tag(C, &tag, false);
    RETURN(true);
}

bool
FlintTable::key_exists(const string &key) const
{
    LOGCALL(DB, bool, "FlintTable::key_exists", key);
    Assert(!key.empty());

    // An oversized key can't exist, so attempting to search for it should fail.
    if (key.size() > FLINT_BTREE_MAX_KEY_LEN) RETURN(false);

    form_key(key);
    RETURN(find(C));
}

bool
FlintTable::read_tag(Cursor_ * C_, string *tag, bool keep_compressed) const
{
    Item_ item(C_[0].p, C_[0].c);

    /* n components to join */
    int n = item.components_of();

    tag->resize(0);
    // max_item_size also includes K1 + I2 + C2 + C2 bytes overhead and the key
    // (which is at least 1 byte long).
    if (n > 1) tag->reserve((max_item_size - (1 + K1 + I2 + C2 + C2)) * n);

    item.append_chunk(tag);
    bool compressed = item.get_compressed();

    for (int i = 2; i <= n; i++) {
	if (!next(C_, 0)) {
	    throw Xapian::DatabaseCorruptError("Unexpected end of table when reading continuation of tag");
	}
	(void)Item_(C_[0].p, C_[0].c).append_chunk(tag);
    }
    // At this point the cursor is on the last item - calling next will move
    // it to the next key (FlintCursor::get_tag() relies on this).
    if (!compressed || keep_compressed) return compressed;

    // FIXME: Perhaps we should we decompress each chunk as we read it so we
    // don't need both the full compressed and uncompressed tags in memory
    // at once.

    string utag;
    // May not be enough for a compressed tag, but it's a reasonable guess.
    utag.reserve(tag->size() + tag->size() / 2);

    Bytef buf[8192];

    lazy_alloc_inflate_zstream();

    // zlib takes a non-const pointer to the input, but doesn't modify it.
    char * non_const_tag = const_cast<char *>(tag->data());
    inflate_zstream->next_in = reinterpret_cast<Byte *>(non_const_tag);
    inflate_zstream->avail_in = uInt(tag->size());

    int err = Z_OK;
    while (err != Z_STREAM_END) {
	inflate_zstream->next_out = buf;
	inflate_zstream->avail_out = uInt(sizeof(buf));
	err = inflate(inflate_zstream, Z_SYNC_FLUSH);
	if (err == Z_BUF_ERROR && inflate_zstream->avail_in == 0) {
	    LOGLINE(DB, "Z_BUF_ERROR - faking checksum of " << inflate_zstream->adler);
	    Bytef header2[4];
	    setint4(header2, 0, inflate_zstream->adler);
	    inflate_zstream->next_in = header2;
	    inflate_zstream->avail_in = 4;
	    err = inflate(inflate_zstream, Z_SYNC_FLUSH);
	    if (err == Z_STREAM_END) break;
	}

	if (err != Z_OK && err != Z_STREAM_END) {
	    if (err == Z_MEM_ERROR) throw std::bad_alloc();
	    string msg = "inflate failed";
	    if (inflate_zstream->msg) {
		msg += " (";
		msg += inflate_zstream->msg;
		msg += ')';
	    }
	    throw Xapian::DatabaseError(msg);
	}

	utag.append(reinterpret_cast<const char *>(buf),
		    inflate_zstream->next_out - buf);
    }
    if (utag.size() != inflate_zstream->total_out) {
	string msg = "compressed tag didn't expand to the expected size: ";
	msg += str(utag.size());
	msg += " != ";
	// OpenBSD's zlib.h uses off_t instead of uLong for total_out.
	msg += str(size_t(inflate_zstream->total_out));
	throw Xapian::DatabaseCorruptError(msg);
    }

    swap(*tag, utag);

    return false;
}

void
FlintTable::set_full_compaction(bool parity)
{
    Assert(writable);

    if (parity) seq_count = 0;
    full_compaction = parity;
}

FlintCursor * FlintTable::cursor_get() const {
    if (handle < 0) {
	if (handle == -2) {
	    FlintTable::throw_database_closed();
	}
	return NULL;
    }
    // FIXME Ick - casting away const is nasty
    return new FlintCursor(const_cast<FlintTable *>(this));
}

/************ B-tree opening and closing ************/

bool
FlintTable::basic_open(bool revision_supplied, flint_revision_number_t revision_)
{
    char ch = 'X'; /* will be 'A' or 'B' */

    {
	const size_t BTREE_BASES = 2;
	string err_msg;
	static const char basenames[BTREE_BASES] = { 'A', 'B' };

	FlintTable_base bases[BTREE_BASES];
	bool base_ok[BTREE_BASES];

	both_bases = true;
	bool valid_base = false;
	{
	    for (size_t i = 0; i < BTREE_BASES; ++i) {
		bool ok = bases[i].read(name, basenames[i], writable, err_msg);
		base_ok[i] = ok;
		if (ok) {
		    valid_base = true;
		} else {
		    both_bases = false;
		}
	    }
	}

	if (!valid_base) {
	    if (handle >= 0) {
		::close(handle);
		handle = -1;
	    }
	    string message = "Error opening table `";
	    message += name;
	    message += "':\n";
	    message += err_msg;
	    throw Xapian::DatabaseOpeningError(message);
	}

	if (revision_supplied) {
	    bool found_revision = false;
	    for (size_t i = 0; i < BTREE_BASES; ++i) {
		if (base_ok[i] && bases[i].get_revision() == revision_) {
		    ch = basenames[i];
		    found_revision = true;
		    break;
		}
	    }
	    if (!found_revision) {
		/* Couldn't open the revision that was asked for.
		 * This shouldn't throw an exception, but should just return
		 * false to upper levels.
		 */
		return false;
	    }
	} else {
	    flint_revision_number_t highest_revision = 0;
	    for (size_t i = 0; i < BTREE_BASES; ++i) {
		if (base_ok[i] && bases[i].get_revision() >= highest_revision) {
		    ch = basenames[i];
		    highest_revision = bases[i].get_revision();
		}
	    }
	}

	FlintTable_base *basep = 0;
	FlintTable_base *other_base = 0;

	for (size_t i = 0; i < BTREE_BASES; ++i) {
	    LOGLINE(DB, "Checking (ch == " << ch << ") against "
		    "basenames[" << i << "] == " << basenames[i]);
	    LOGLINE(DB, "bases[" << i << "].get_revision() == " <<
		    bases[i].get_revision());
	    LOGLINE(DB, "base_ok[" << i << "] == " << base_ok[i]);
	    if (ch == basenames[i]) {
		basep = &bases[i];

		// FIXME: assuming only two bases for other_base
		size_t otherbase_num = 1-i;
		if (base_ok[otherbase_num]) {
		    other_base = &bases[otherbase_num];
		}
		break;
	    }
	}
	Assert(basep);

	/* basep now points to the most recent base block */

	/* Avoid copying the bitmap etc. - swap contents with the base
	 * object in the vector, since it'll be destroyed anyway soon.
	 */
	base.swap(*basep);

	revision_number =  base.get_revision();
	block_size =       base.get_block_size();
	root =             base.get_root();
	level =            base.get_level();
	//bit_map_size =     basep->get_bit_map_size();
	item_count =       base.get_item_count();
	faked_root_block = base.get_have_fakeroot();
	sequential =       base.get_sequential();

	if (other_base != 0) {
	    latest_revision_number = other_base->get_revision();
	    if (revision_number > latest_revision_number)
		latest_revision_number = revision_number;
	} else {
	    latest_revision_number = revision_number;
	}
    }

    /* kt holds constructed items as well as keys */
    kt = Item_wr_(zeroed_new(block_size));

    set_max_item_size(BLOCK_CAPACITY);

    base_letter = ch;

    /* ready to open the main file */

    return true;
}

void
FlintTable::read_root()
{
    if (faked_root_block) {
	/* root block for an unmodified database. */
	Byte * p = C[0].p;
	Assert(p);

	/* clear block - shouldn't be necessary, but is a bit nicer,
	 * and means that the same operations should always produce
	 * the same database. */
	memset(p, 0, block_size);

	int o = block_size - I2 - K1 - C2 - C2;
	Item_wr_(p + o).fake_root_item();

	setD(p, DIR_START, o);         // its directory entry
	SET_DIR_END(p, DIR_START + D2);// the directory size

	o -= (DIR_START + D2);
	SET_MAX_FREE(p, o);
	SET_TOTAL_FREE(p, o);
	SET_LEVEL(p, 0);

	if (!writable) {
	    /* reading - revision number doesn't matter as long as
	     * it's not greater than the current one. */
	    SET_REVISION(p, 0);
	    C[0].n = 0;
	} else {
	    /* writing - */
	    SET_REVISION(p, latest_revision_number + 1);
	    C[0].n = base.next_free_block();
	}
    } else {
	/* using a root block stored on disk */
	block_to_cursor(C, level, root);

	if (REVISION(C[level].p) > revision_number) set_overwritten();
	/* although this is unlikely */
    }
}

bool
FlintTable::do_open_to_write(bool revision_supplied,
			     flint_revision_number_t revision_,
			     bool create_db)
{
    if (handle == -2) {
	FlintTable::throw_database_closed();
    }
    int flags = O_RDWR | O_BINARY;
    if (create_db) flags |= O_CREAT | O_TRUNC;
    handle = ::open((name + "DB").c_str(), flags, 0666);
    if (handle < 0) {
	// lazy doesn't make a lot of sense with create_db anyway, but ENOENT
	// with O_CREAT means a parent directory doesn't exist.
	if (lazy && !create_db && errno == ENOENT) {
	    revision_number = revision_;
	    return true;
	}
	string message(create_db ? "Couldn't create " : "Couldn't open ");
	message += name;
	message += "DB read/write: ";
	message += strerror(errno);
	throw Xapian::DatabaseOpeningError(message);
    }

    if (!basic_open(revision_supplied, revision_)) {
	::close(handle);
	handle = -1;
	if (!revision_supplied) {
	    throw Xapian::DatabaseOpeningError("Failed to open for writing");
	}
	/* When the revision is supplied, it's not an exceptional
	 * case when open failed, so we just return false here.
	 */
	return false;
    }

    writable = true;

    for (int j = 0; j <= level; j++) {
	C[j].n = BLK_UNUSED;
	C[j].p = new Byte[block_size];
	if (C[j].p == 0) {
	    throw std::bad_alloc();
	}
    }
    split_p = new Byte[block_size];
    if (split_p == 0) {
	throw std::bad_alloc();
    }
    read_root();

    buffer = zeroed_new(block_size);

    changed_n = 0;
    changed_c = DIR_START;
    seq_count = SEQ_START_POINT;

    return true;
}

FlintTable::FlintTable(const char * tablename_, const string & path_,
		       bool readonly_, int compress_strategy_, bool lazy_)
	: tablename(tablename_),
	  revision_number(0),
	  item_count(0),
	  block_size(0),
	  latest_revision_number(0),
	  both_bases(false),
	  base_letter('A'),
	  faked_root_block(true),
	  sequential(true),
	  handle(-1),
	  level(0),
	  root(0),
	  kt(0),
	  buffer(0),
	  base(),
	  name(path_),
	  seq_count(0),
	  changed_n(0),
	  changed_c(0),
	  max_item_size(0),
	  Btree_modified(false),
	  full_compaction(false),
	  writable(!readonly_),
	  cursor_created_since_last_modification(false),
	  cursor_version(0),
	  split_p(0),
	  compress_strategy(compress_strategy_),
	  deflate_zstream(NULL),
	  inflate_zstream(NULL),
	  lazy(lazy_)
{
    LOGCALL_VOID(DB, "FlintTable::FlintTable", tablename_ | path_ | readonly_ | compress_strategy_ | lazy_);
}

bool
FlintTable::really_empty() const
{
    if (handle < 0) {
	if (handle == -2) {
	    FlintTable::throw_database_closed();
	}
	return true;
    }
    FlintCursor cur(const_cast<FlintTable*>(this));
    cur.find_entry(string());
    return !cur.next();
}

void
FlintTable::lazy_alloc_deflate_zstream() const {
    if (usual(deflate_zstream)) {
	if (usual(deflateReset(deflate_zstream) == Z_OK)) return;
	// Try to recover by deleting the stream and starting from scratch.
	delete deflate_zstream;
    }

    deflate_zstream = new z_stream;

    deflate_zstream->zalloc = Z_NULL;
    deflate_zstream->zfree = Z_NULL;
    deflate_zstream->opaque = Z_NULL;

    // -15 means raw deflate with 32K LZ77 window (largest)
    // memLevel 9 is the highest (8 is default)
    int err;
    err = deflateInit2(deflate_zstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
		       -15, 9, compress_strategy);
    if (rare(err != Z_OK)) {
	if (err == Z_MEM_ERROR) {
	    delete deflate_zstream;
	    deflate_zstream = 0;
	    throw std::bad_alloc();
	}
	string msg = "deflateInit2 failed (";
	if (deflate_zstream->msg) {
	    msg += deflate_zstream->msg;
	} else {
	    msg += str(err);
	}
	msg += ')';
	delete deflate_zstream;
	deflate_zstream = 0;
	throw Xapian::DatabaseError(msg);
    }
}

void
FlintTable::lazy_alloc_inflate_zstream() const {
    if (usual(inflate_zstream)) {
	if (usual(inflateReset(inflate_zstream) == Z_OK)) return;
	// Try to recover by deleting the stream and starting from scratch.
	delete inflate_zstream;
    }

    inflate_zstream = new z_stream;

    inflate_zstream->zalloc = Z_NULL;
    inflate_zstream->zfree = Z_NULL;
    inflate_zstream->opaque = Z_NULL;

    inflate_zstream->next_in = Z_NULL;
    inflate_zstream->avail_in = 0;

    int err = inflateInit2(inflate_zstream, -15);
    if (rare(err != Z_OK)) {
	if (err == Z_MEM_ERROR) {
	    delete inflate_zstream;
	    inflate_zstream = 0;
	    throw std::bad_alloc();
	}
	string msg = "inflateInit2 failed (";
	if (inflate_zstream->msg) {
	    msg += inflate_zstream->msg;
	} else {
	    msg += str(err);
	}
	msg += ')';
	delete inflate_zstream;
	inflate_zstream = 0;
	throw Xapian::DatabaseError(msg);
    }
}

bool
FlintTable::exists() const {
    LOGCALL(DB, bool, "FlintTable::exists", NO_ARGS);
    return (file_exists(name + "DB") &&
	    (file_exists(name + "baseA") || file_exists(name + "baseB")));
}

void
FlintTable::erase()
{
    LOGCALL_VOID(DB, "FlintTable::erase", NO_ARGS);
    close();

    (void)io_unlink(name + "baseA");
    (void)io_unlink(name + "baseB");
    (void)io_unlink(name + "DB");
}

void
FlintTable::set_block_size(unsigned int block_size_)
{
    LOGCALL_VOID(DB, "FlintTable::set_block_size", block_size_);
    // Block size must in the range 2048..BYTE_PAIR_RANGE, and a power of two.
    if (block_size_ < 2048 || block_size_ > BYTE_PAIR_RANGE ||
	(block_size_ & (block_size_ - 1)) != 0) {
	block_size_ = FLINT_DEFAULT_BLOCK_SIZE;
    }
    block_size = block_size_;
}

void
FlintTable::create_and_open(unsigned int block_size_)
{
    LOGCALL_VOID(DB, "FlintTable::create_and_open", block_size_);
    if (handle == -2) {
	FlintTable::throw_database_closed();
    }
    Assert(writable);
    close();

    if (block_size_ == 0) abort();
    set_block_size(block_size_);

    // FIXME: it would be good to arrange that this works such that there's
    // always a valid table in place if you run create_and_open() on an
    // existing table.

    /* write initial values to files */

    /* create the base file */
    FlintTable_base base_;
    base_.set_revision(revision_number);
    base_.set_block_size(block_size_);
    base_.set_have_fakeroot(true);
    base_.set_sequential(true);
    base_.write_to_file(name + "baseA", 'A', string(), -1, NULL);

    /* remove the alternative base file, if any */
    (void)io_unlink(name + "baseB");

    // Any errors are thrown if revision_supplied is false.
    (void)do_open_to_write(false, 0, true);
}

FlintTable::~FlintTable() {
    LOGCALL_VOID(DB, "FlintTable::~FlintTable", NO_ARGS);
    FlintTable::close();

    if (deflate_zstream) {
	// Errors which we care about have already been handled, so just ignore
	// any which get returned here.
	(void) deflateEnd(deflate_zstream);
	delete deflate_zstream;
    }

    if (inflate_zstream) {
	// Errors which we care about have already been handled, so just ignore
	// any which get returned here.
	(void) inflateEnd(inflate_zstream);
	delete inflate_zstream;
    }
}

void FlintTable::close(bool permanent) {
    LOGCALL_VOID(DB, "FlintTable::close", NO_ARGS);

    if (handle >= 0) {
	// If an error occurs here, we just ignore it, since we're just
	// trying to free everything.
	(void)::close(handle);
	handle = -1;
    }

    if (permanent) {
	handle = -2;
	// Don't delete the resources in the table, since they may
	// still be used to look up cached content.
	return;
    }

    for (int j = level; j >= 0; j--) {
	delete [] C[j].p;
	C[j].p = 0;
    }
    delete [] split_p;
    split_p = 0;

    delete [] kt.get_address();
    kt = 0;
    delete [] buffer;
    buffer = 0;
}

void
FlintTable::flush_db()
{
    LOGCALL_VOID(DB, "FlintTable::flush_db", NO_ARGS);
    Assert(writable);
    if (handle < 0) {
	if (handle == -2) {
	    FlintTable::throw_database_closed();
	}
	return;
    }

    for (int j = level; j >= 0; j--) {
	if (C[j].rewrite) {
	    write_block(C[j].n, C[j].p);
	}
    }

    if (Btree_modified) {
	faked_root_block = false;
    }
}

void
FlintTable::commit(flint_revision_number_t revision, int changes_fd,
		   const string * changes_tail)
{
    LOGCALL_VOID(DB, "FlintTable::commit", revision | changes_fd | changes_tail);
    Assert(writable);

    if (revision <= revision_number) {
	throw Xapian::DatabaseError("New revision too low");
    }

    if (handle < 0) {
	if (handle == -2) {
	    FlintTable::throw_database_closed();
	}
	latest_revision_number = revision_number = revision;
	return;
    }

    try {
	if (faked_root_block) {
	    /* We will use a dummy bitmap. */
	    base.clear_bit_map();
	}

	base.set_revision(revision);
	base.set_root(C[level].n);
	base.set_level(level);
	base.set_item_count(item_count);
	base.set_have_fakeroot(faked_root_block);
	base.set_sequential(sequential);

	base_letter = other_base_letter();

	both_bases = true;
	latest_revision_number = revision_number = revision;
	root = C[level].n;

	Btree_modified = false;

	for (int i = 0; i < BTREE_CURSOR_LEVELS; ++i) {
	    C[i].n = BLK_UNUSED;
	    C[i].c = -1;
	    C[i].rewrite = false;
	}

	// Do this as late as possible to allow maximum time for writes to be
	// committed.
	if (!io_sync(handle)) {
	    (void)::close(handle);
	    handle = -1;
	    throw Xapian::DatabaseError("Can't commit new revision - failed to flush DB to disk");
	}

	// Save to "<table>.tmp" and then rename to "<table>.base<letter>" so
	// that a reader can't try to read a partially written base file.
	string tmp = name;
	tmp += "tmp";
	string basefile = name;
	basefile += "base";
	basefile += char(base_letter);
	base.write_to_file(tmp, base_letter, tablename, changes_fd, changes_tail);
#if defined __WIN32__
	if (msvc_posix_rename(tmp.c_str(), basefile.c_str()) < 0)
#else
	if (rename(tmp.c_str(), basefile.c_str()) < 0)
#endif
	{
	    // With NFS, rename() failing may just mean that the server crashed
	    // after successfully renaming, but before reporting this, and then
	    // the retried operation fails.  So we need to check if the source
	    // file still exists, which we do by calling unlink(), since we want
	    // to remove the temporary file anyway.
	    int saved_errno = errno;
	    if (unlink(tmp) == 0 || errno != ENOENT) {
		string msg("Couldn't update base file ");
		msg += basefile;
		msg += ": ";
		msg += strerror(saved_errno);
		throw Xapian::DatabaseError(msg);
	    }
	}
	base.commit();

	read_root();

	changed_n = 0;
	changed_c = DIR_START;
	seq_count = SEQ_START_POINT;
    } catch (...) {
	FlintTable::close();
	throw;
    }
}

void
FlintTable::write_changed_blocks(int changes_fd)
{
    Assert(changes_fd >= 0);
    if (handle < 0) return;
    if (faked_root_block) return;

    string buf;
    buf += F_pack_uint(2u); // Indicate the item is a list of blocks
    buf += F_pack_uint(strlen(tablename));
    buf += tablename;
    buf += F_pack_uint(block_size);
    io_write(changes_fd, buf.data(), buf.size());

    // Compare the old and new bitmaps to find blocks which have changed, and
    // write them to the file descriptor.
    uint4 n = 0;
    Byte * p = new Byte[block_size];
    try {
	base.calculate_last_block();
	while (base.find_changed_block(&n)) {
	    buf = F_pack_uint(n + 1);
	    io_write(changes_fd, buf.data(), buf.size());

	    // Read block n.
	    read_block(n, p);

	    // Write block n to the file.
	    io_write(changes_fd, reinterpret_cast<const char *>(p), block_size);
	    ++n;
	}
	delete[] p;
	p = 0;
    } catch (...) {
	delete[] p;
	throw;
    }
    buf = F_pack_uint(0u);
    io_write(changes_fd, buf.data(), buf.size());
}

void
FlintTable::cancel()
{
    LOGCALL_VOID(DB, "FlintTable::cancel", NO_ARGS);
    Assert(writable);

    if (handle < 0) {
	if (handle == -2) {
	    FlintTable::throw_database_closed();
	}
	latest_revision_number = revision_number; // FIXME: we can end up reusing a revision if we opened a btree at an older revision, start to modify it, then cancel...
	return;
    }

    // This causes problems: if (!Btree_modified) return;

    string err_msg;
    if (!base.read(name, base_letter, writable, err_msg)) {
	throw Xapian::DatabaseCorruptError(string("Couldn't reread base ") + base_letter);
    }

    revision_number =  base.get_revision();
    block_size =       base.get_block_size();
    root =             base.get_root();
    level =            base.get_level();
    //bit_map_size =     basep->get_bit_map_size();
    item_count =       base.get_item_count();
    faked_root_block = base.get_have_fakeroot();
    sequential =       base.get_sequential();

    latest_revision_number = revision_number; // FIXME: we can end up reusing a revision if we opened a btree at an older revision, start to modify it, then cancel...

    Btree_modified = false;

    for (int j = 0; j <= level; j++) {
	C[j].n = BLK_UNUSED;
	C[j].rewrite = false;
    }
    read_root();

    changed_n = 0;
    changed_c = DIR_START;
    seq_count = SEQ_START_POINT;
}

/************ B-tree reading ************/

bool
FlintTable::do_open_to_read(bool revision_supplied, flint_revision_number_t revision_)
{
    if (handle == -2) {
	FlintTable::throw_database_closed();
    }
    handle = ::open((name + "DB").c_str(), O_RDONLY | O_BINARY);
    if (handle < 0) {
	if (lazy) {
	    // This table is optional when reading!
	    revision_number = revision_;
	    return true;
	}
	string message("Couldn't open ");
	message += name;
	message += "DB to read: ";
	message += strerror(errno);
	throw Xapian::DatabaseOpeningError(message);
    }

    if (!basic_open(revision_supplied, revision_)) {
	::close(handle);
	handle = -1;
	if (revision_supplied) {
	    // The requested revision was not available.
	    // This could be because the database was modified underneath us, or
	    // because a base file is missing.  Return false, and work out what
	    // the problem was at a higher level.
	    return false;
	}
	throw Xapian::DatabaseOpeningError("Failed to open table for reading");
    }

    for (int j = 0; j <= level; j++) {
	C[j].n = BLK_UNUSED;
	C[j].p = new Byte[block_size];
	if (C[j].p == 0) {
	    throw std::bad_alloc();
	}
    }

    read_root();
    return true;
}

void
FlintTable::open()
{
    LOGCALL_VOID(DB, "FlintTable::open", NO_ARGS);
    LOGLINE(DB, "opening at path " << name);
    close();

    if (!writable) {
	// Any errors are thrown if revision_supplied is false
	(void)do_open_to_read(false, 0);
	return;
    }

    // Any errors are thrown if revision_supplied is false.
    (void)do_open_to_write(false, 0);
}

bool
FlintTable::open(flint_revision_number_t revision)
{
    LOGCALL(DB, bool, "FlintTable::open", revision);
    LOGLINE(DB, "opening for particular revision at path " << name);
    close();

    if (!writable) {
	if (do_open_to_read(true, revision)) {
	    AssertEq(revision_number, revision);
	    RETURN(true);
	} else {
	    close();
	    RETURN(false);
	}
    }

    if (!do_open_to_write(true, revision)) {
	// Can't open at the requested revision.
	close();
	RETURN(false);
    }

    AssertEq(revision_number, revision);
    RETURN(true);
}

bool
FlintTable::prev_for_sequential(Cursor_ * C_, int /*dummy*/) const
{
    int c = C_[0].c;
    if (c == DIR_START) {
	Byte * p = C_[0].p;
	Assert(p);
	uint4 n = C_[0].n;
	while (true) {
	    if (n == 0) return false;
	    n--;
	    if (writable) {
		if (n == C[0].n) {
		    // Block is a leaf block in the built-in cursor
		    // (potentially in modified form).
		    memcpy(p, C[0].p, block_size);
		} else {
		    // Blocks in the built-in cursor may not have been written
		    // to disk yet, so we have to check that the block number
		    // isn't in the built-in cursor or we'll read an
		    // uninitialised block (for which GET_LEVEL(p) will
		    // probably return 0).
		    int j;
		    for (j = 1; j <= level; ++j) {
			if (n == C[j].n) break;
		    }
		    if (j <= level) continue;

		    // Block isn't in the built-in cursor, so the form on disk
		    // is valid, so read it to check if it's the next level 0
		    // block.
		    read_block(n, p);
		}
	    } else {
		read_block(n, p);
	    }
	    
	    if (writable) AssertEq(revision_number, latest_revision_number);
	    if (REVISION(p) > revision_number + writable) {
		set_overwritten();
		return false;
	    }
	    if (GET_LEVEL(p) == 0) break;
	}
	c = DIR_END(p);
	C_[0].n = n;
    }
    c -= D2;
    C_[0].c = c;
    return true;
}

bool
FlintTable::next_for_sequential(Cursor_ * C_, int /*dummy*/) const
{
    Byte * p = C_[0].p;
    Assert(p);
    int c = C_[0].c;
    c += D2;
    Assert((unsigned)c < block_size);
    if (c == DIR_END(p)) {
	uint4 n = C_[0].n;
	while (true) {
	    n++;
	    if (n > base.get_last_block()) return false;
	    if (writable) {
		if (n == C[0].n) {
		    // Block is a leaf block in the built-in cursor
		    // (potentially in modified form).
		    memcpy(p, C[0].p, block_size);
		} else {
		    // Blocks in the built-in cursor may not have been written
		    // to disk yet, so we have to check that the block number
		    // isn't in the built-in cursor or we'll read an
		    // uninitialised block (for which GET_LEVEL(p) will
		    // probably return 0).
		    int j;
		    for (j = 1; j <= level; ++j) {
			if (n == C[j].n) break;
		    }
		    if (j <= level) continue;

		    // Block isn't in the built-in cursor, so the form on disk
		    // is valid, so read it to check if it's the next level 0
		    // block.
		    read_block(n, p);
		}
	    } else {
		read_block(n, p);
	    }
	    if (writable) AssertEq(revision_number, latest_revision_number);
	    if (REVISION(p) > revision_number + writable) {
		set_overwritten();
		return false;
	    }
	    if (GET_LEVEL(p) == 0) break;
	}
	c = DIR_START;
	C_[0].n = n;
    }
    C_[0].c = c;
    return true;
}

bool
FlintTable::prev_default(Cursor_ * C_, int j) const
{
    Byte * p = C_[j].p;
    int c = C_[j].c;
    Assert(c >= DIR_START);
    Assert((unsigned)c < block_size);
    Assert(c <= DIR_END(p));
    if (c == DIR_START) {
	if (j == level) return false;
	if (!prev_default(C_, j + 1)) return false;
	c = DIR_END(p);
    }
    c -= D2;
    C_[j].c = c;
    if (j > 0) {
	block_to_cursor(C_, j - 1, Item_(p, c).block_given_by());
    }
    return true;
}

bool
FlintTable::next_default(Cursor_ * C_, int j) const
{
    Byte * p = C_[j].p;
    int c = C_[j].c;
    Assert(c >= DIR_START);
    c += D2;
    Assert((unsigned)c < block_size);
    // Sometimes c can be DIR_END(p) + 2 here it appears...
    if (c >= DIR_END(p)) {
	if (j == level) return false;
	if (!next_default(C_, j + 1)) return false;
	c = DIR_START;
    }
    C_[j].c = c;
    if (j > 0) {
	block_to_cursor(C_, j - 1, Item_(p, c).block_given_by());
#ifdef BTREE_DEBUG_FULL
	printf("Block in FlintTable:next_default");
	report_block_full(j - 1, C_[j - 1].n, C_[j - 1].p);
#endif /* BTREE_DEBUG_FULL */
    }
    return true;
}

void
FlintTable::throw_database_closed()
{
    throw Xapian::DatabaseError("Database has been closed");
}

/** Compares this key with key2.

   The result is true if this key precedes key2. The comparison is for byte
   sequence collating order, taking lengths into account. So if the keys are
   made up of lower case ASCII letters we get alphabetical ordering.

   Now remember that items are added into the B-tree in fastest time
   when they are preordered by their keys. This is therefore the piece
   of code that needs to be followed to arrange for the preordering.

   This is complicated by the fact that keys have two parts - a value
   and then a count.  We first compare the values, and only if they
   are equal do we compare the counts.
*/

bool Key_::operator<(Key_ key2) const
{
    LOGCALL(DB, bool, "Key_::operator<", static_cast<const void*>(key2.p));
    int key1_len = length();
    int key2_len = key2.length();
    if (key1_len == key2_len) {
	// The keys are the same length, so we can compare the counts
	// in the same operation since they're stored as 2 byte
	// bigendian numbers.
	RETURN(memcmp(p + K1, key2.p + K1, key1_len + C2) < 0);
    }

    int k_smaller = (key2_len < key1_len ? key2_len : key1_len);

    // Compare the common part of the keys
    int diff = memcmp(p + K1, key2.p + K1, k_smaller);
    if (diff != 0) RETURN(diff < 0);

    // We dealt with the "same length" case above so we never need to check
    // the count here.
    RETURN(key1_len < key2_len);
}

bool Key_::operator==(Key_ key2) const
{
    LOGCALL(DB, bool, "Key_::operator==", static_cast<const void*>(key2.p));
    int key1_len = length();
    if (key1_len != key2.length()) return false;
    // The keys are the same length, so we can compare the counts
    // in the same operation since they're stored as 2 byte
    // bigendian numbers.
    RETURN(memcmp(p + K1, key2.p + K1, key1_len + C2) == 0);
}
