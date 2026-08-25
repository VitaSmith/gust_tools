/*
  gust_enc - Encoder/Decoder for Gust (Koei/Tecmo) .e files
  Copyright © 2019-2021 - VitaSmith
  Prime number computation copyright © 2001-2003 - Stephane Carrez

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*

  Note to Gust: I love you. And I love playing your games.

  And yet I can't remember the last time I've had as much fun as I did
  breaking this weird unscrambling/decompression algorithm of yours.

  So thank you very much for (unwillingly) creating one of the best
  detective games ever!

  And _please_ don't try to fight modders: We are on your side!

  Sincerely,

  -- VitaSmith, 2019-10-02

 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "utf8.h"
#include "util.h"
#include "parson.h"

#define E_HEADER_SIZE       0x10
#define E_FOOTER_SIZE       0x10

// Both of these are prime numbers
#define RANDOM_CONSTANT     0x3b9a73c9
#define RANDOM_INCREMENT    0x2f09
#define MB                  (1024 * 1024)

// For Sophie's GrowData.xml.e
//#define VALIDATE_CHECKSUM   0x52ccbbab
// For Sophie's Marquee.xml.e
//#define VALIDATE_CHECKSUM   0x92ca8716

//#define CREATE_EXTRA_FILES

//#define USE_GLAZED

typedef struct {
    uint32_t main[3];
    uint32_t table[3];
    uint32_t length[3];
    uint16_t fence;
} seed_data;

// Bitmap list prime numbers below a specific value
static uint8_t* prime_list = NULL;
static uint32_t random_seed[2];
// TODO: Use endianness handling from util.[h/c]
static bool is_big_endian = true;
#define getdata16(x) (is_big_endian ? getbe16(x) : getle16(x))
#define getdata32(x) (is_big_endian ? getbe32(x) : getle32(x))
#define setdata16(x, v) (is_big_endian ? setbe16(x, v): setle16(x, v))
#define setdata32(x, v) (is_big_endian ? setbe32(x, v): setle32(x, v))

/*
 * Helper functions to generate predictible semirandom numbers
 */
static __inline void init_random(uint32_t r0, uint32_t r1)
{
    random_seed[0] = RANDOM_CONSTANT + r0;
    random_seed[1] = r1;
}

static __inline uint16_t get_random_u15(void)
{
    random_seed[1] = random_seed[0] * random_seed[1] + RANDOM_INCREMENT;
    return (random_seed[1] >> 16) & 0x7fff;
}

static __inline uint16_t get_random_u16(void)
{
    random_seed[1] = random_seed[0] * random_seed[1] + RANDOM_INCREMENT;
    return random_seed[1] >> 16;
}

/*
 * Stupid sexy scramblers ("Feels like I'm reading nothing at all!")
 *
 * All the scramblers below are seeded scramblers that derive values from the formula
 * seed[1] = seed[0] * seed[1] + 0x2f09, with seed[0] being prime number 0x3b9a73c9
 * (or a variation thereof) and seed[1] another 16-bit prime number.
 *
 * From there, they only differ in the manner with which they use the updated seed.
 */

// Scramble individual bits between two semi-random bit positions within a slice.
static bool bit_scrambler(uint8_t* chunk, uint32_t chunk_size, uint32_t slice_size,
                          bool descramble)
{
    // Table_size needs to be 8 * slice_size, to encompass all individual bit positions
    uint32_t x, table_size = slice_size << 3;

    uint16_t* base_table = calloc(table_size, sizeof(uint16_t));
    uint16_t* scrambling_table = calloc(table_size, sizeof(uint16_t));
    if ((table_size < 4) || (base_table == NULL) || (scrambling_table == NULL)) {
        free(base_table);
        free(scrambling_table);
        return false;
    }

    uint8_t* max_chunk = &chunk[chunk_size];
    while (chunk < max_chunk) {
        // Make sure we don't overflow our table, else we're going to pick
        // bits located outside our chunk
        table_size = min(table_size, chunk_size << 3);
        // Create a base table of incremental 16-bit values
        for (uint32_t i = 0; i < table_size; i++)
            base_table[i] = (uint16_t)i;

        // Now create a scrambled table from the above
        for (uint32_t i = 0; i < table_size; i++) {
            // Translate this semi-random value to a base_table index we haven't used yet
            x = get_random_u15() % (table_size - i);
            scrambling_table[i] = base_table[x];
            // Now remove the value we used from base_table
            memmove(&base_table[x], &base_table[x + 1], (size_t)(table_size - i - x) * 2);
        }

        // This scrambler uses a pair of byte and bit positions that are derived from
        // values picked in the scrambling table (>>3 for byte pos and &7 for bit pos)
        // From there, the scrambler swaps the bits at position p0.b0 and p1.b1.
        // To perform the reverse operation, the scrambling table must be parsed in the
        // reverse direction since sequential bit swaps are not commutative.
        uint8_t p0, p1, b0, b1, v0, v1;
        int32_t start_value = descramble ? 0 : (int32_t)min(table_size, chunk_size << 3) - 2;
        int32_t increment = descramble ? +2 : -2;
        for (int32_t i = start_value; (i >= 0) && (i < (int32_t)min(table_size, chunk_size << 3)); i += increment) {
            p0 = (uint8_t)(scrambling_table[i] >> 3);
            b0 = (uint8_t)(scrambling_table[i] & 7);
// Don't bug me Microsoft, you're wrong
#pragma warning(push)
#pragma warning(disable:6385)
            p1 = (uint8_t)(scrambling_table[i + 1] >> 3);
            b1 = (uint8_t)(scrambling_table[i + 1] & 7);
#pragma warning(pop)
            // Keep the bit values
            v0 = (chunk[p0] & (1 << b0)) >> b0;
            v1 = (chunk[p1] & (1 << b1)) >> b1;
            // Filter out bit b0 from the byte at position b
            chunk[p0] &= ~(1 << b0);
            chunk[p0] |= v1 << b0;
            chunk[p1] &= ~(1 << b1);
            chunk[p1] |= v0 << b1;
        }

        chunk = &chunk[slice_size];
        chunk_size -= slice_size;
    }

    free(base_table);
    free(scrambling_table);
    return true;
}

// Sequentially scramble bytes by adding the updated seed and, depending on whether
// the modulo with the current seed falls above or below a "fence", XORing the seed.
static bool fenced_scrambler(uint8_t* buf, uint32_t buf_size, uint16_t fence,
                             bool descramble, bool extra_fudge)
{
    for (uint32_t i = 0; i < buf_size; i += 2) {
        uint16_t x = get_random_u15();
        uint16_t w = getdata16(&buf[i]);
        // The fence is a 12-bit prime number
        if (descramble) {
            if (x % (fence * 2) >= fence)
                w ^= extra_fudge ? get_random_u15() : x;
            w -= x;
        } else {
            w += x;
            if (x % (fence * 2) >= fence)
                w ^= extra_fudge ? get_random_u15() : x;
        }
        setdata16(&buf[i], w);
    }
    return true;
}

// Sequentially scramble bytes by XORing them with a set of 3 rotated seeds.
static bool rotating_scrambler(uint8_t* buf, uint32_t buf_size, const seed_data* seeds)
{
    // We're updating seed values in the table, so make sure we work on a copy
    uint32_t seed_table[3] = { seeds->table[0], seeds->table[1], seeds->table[2] };
    uint32_t seed_index = 0;
    uint32_t seed_switch_fudge = 0;
    uint32_t processed_for_this_seed = 0;
    for (uint32_t i = 0; i < buf_size; i++) {
        buf[i] ^= get_random_u16();
        if (++processed_for_this_seed >= seeds->length[seed_index] + seed_switch_fudge) {
            seed_table[seed_index++] = random_seed[1];
            if (seed_index >= array_size(seed_table)) {
                seed_index = 0;
                seed_switch_fudge++;
            }
            random_seed[1] = seed_table[seed_index];
            processed_for_this_seed = 0;
        }
    }
    return true;
}

/*
  The following functions deal with the compression algorithm used by Gust, which
  looks like a derivative of LZSS that I am calling 'Glaze', for "Gust Lempel–Ziv".
 */
typedef struct {
    uint8_t* buffer;
    uint32_t size;
    uint32_t pos;
    int getbits_buffer;
    int getbits_mask;
} getbits_ctx;

#define GETBITS_EOF 0xffffffff
static uint32_t getbits(getbits_ctx* ctx, int n)
{
    int x = 0;

    for (int i = 0; i < n; i++) {
        if (ctx->getbits_mask == 0x00) {
            if (ctx->pos >= ctx->size)
                return GETBITS_EOF;
            ctx->getbits_buffer = ctx->buffer[ctx->pos++];
            ctx->getbits_mask = 0x80;
        }
        x <<= 1;
        if (ctx->getbits_buffer & ctx->getbits_mask)
            x++;
        ctx->getbits_mask >>= 1;
    }

    return x;
}

// Boy with extended open hand, looking at butterfly: "Is this Huffman encoding?"
static uint8_t* build_code_table(uint8_t* bitstream, uint32_t bitstream_length)
{
    uint32_t code_table_length = getdata32(bitstream);
    if (code_table_length > 256 * MB) {
        fprintf(stderr, "ERROR: Glaze code table length is too large\n");
        return NULL;
    }
    uint8_t* code_table = malloc(code_table_length);
    if (code_table == NULL)
        return NULL;
    getbits_ctx ctx = { 0 };
    ctx.buffer = &bitstream[sizeof(uint32_t)];
    ctx.size = bitstream_length - sizeof(uint32_t);

    for (uint32_t c = getbits(&ctx, 1), i = 0; i < code_table_length; c = getbits(&ctx, 1), i++) {
        if (c == GETBITS_EOF) {
            break;
        } else if (c == 1) {
            // Bit sequence starts with 1 -> emit code 0x01
            code_table[i] = 1;
        } else {
            // Bit sequence starts with 0 -> get the length of code and emit it
            int code_len = 0;
            while ((++code_len < 8) && ((c = getbits(&ctx, 1)) == 0));
            if (c == GETBITS_EOF)
                break;
            if (code_len < 8)
                code_table[i] = (uint8_t)((c << code_len) | getbits(&ctx, code_len));
            else
                code_table[i] = 0;
        }
    }

    return code_table;
}

// Uncompress a glaze compressed buffer
static uint32_t unglaze(uint8_t* src, uint32_t src_length, uint8_t* dst, uint32_t dst_length)
{
    uint32_t dec_length = getdata32(src);
    src = &src[sizeof(uint32_t)];
    if (dec_length > dst_length) {
        fprintf(stderr, "ERROR: Glaze decompression buffer is too small\n");
        return 0;
    }

    uint32_t bitstream_length = getdata32(src);
    src = &src[sizeof(uint32_t)];
    if (bitstream_length <= sizeof(uint32_t)) {
        fprintf(stderr, "ERROR: Glaze decompression bitstream is too small\n");
        return 0;
    }

    uint32_t chk_length = bitstream_length + sizeof(uint32_t);
    if (chk_length >= src_length) {
        fprintf(stderr, "ERROR: Glaze decompression bitstream is too large\n");
        return 0;
    }

    uint32_t code_len = getdata32(src);
    uint8_t* code_table = build_code_table(src, bitstream_length);
    if (code_table == NULL)
        return 0;

    uint8_t* dict = &src[bitstream_length];
    uint32_t dict_len = getdata32(dict);
    dict = &dict[sizeof(uint32_t)];
    chk_length += dict_len + sizeof(uint32_t);
    if (chk_length >= src_length) {
        fprintf(stderr, "ERROR: Glaze decompression dictionary is too large\n");
        free(code_table);
        return 0;
    }

    uint8_t* len = &dict[dict_len];
    uint8_t* max_dict = len;
    uint32_t len_len = getdata32(len);
    len = &len[sizeof(uint32_t)];
    uint8_t* max_len = &len[len_len];
    chk_length += len_len + sizeof(uint32_t);
    if (chk_length >= src_length) {
        fprintf(stderr, "ERROR: Glaze decompression length table is too large\n");
        free(code_table);
        return 0;
    }

    int l, d;
    uint8_t* dst_max = &dst[dec_length];
    uint8_t* code = code_table;
    uint8_t* max_code = &code_table[code_len];
    while (dst < dst_max) {
        // Sanity checks
        if ((dict > max_dict) || (len > max_len) || (code > max_code)) {
            fprintf(stderr, "ERROR: Glaze decompression overflow\n");
            free(code_table);
            return 0;
        }
        switch (*code++) {
        case 0x01:  // 1-byte code
            // Copy one byte
            *dst++ = *dict++;
            break;
        case 0x02:  // 2-byte code
            // Duplicate one byte from pos -d where d is provided by the code table
            d = *code++;
            *dst++ = dst[-d];
            break;
        case 0x03:  // 3-byte code
            // Duplicate l bytes from position -(d + l) where both d and l are provided by the code table
            d = *code++;
            l = *code++;
            d += l;
            for (int i = ++l; i > 0; i--)
                *dst++ = dst[-d];
            break;
        case 0x04:  // 2-byte code
            // Duplicate l bytes from position -(d + l) where l is provided by the code table and d by the source
            l = *code++;
            d = *dict++ + l;
            for (int i = ++l; i > 0; i--)
                *dst++ = dst[-d];
            break;
        case 0x05:  // 3-byte code
            // Same as above except with a 16-bit distance where the MSB is provided by the code table and LSB by the source
            d = *code++ << 8 | *dict++;
            l = *code++;
            d += l;
            for (int i = ++l; i > 0; i--)
                *dst++ = dst[-d];
            break;
        case 0x06:  // 2-byte code
            // Copy l + 8 bytes from source where l is provided by the code table
            l = *code++ + 8;
            for (int i = l; i > 0; i--) {
                *dst++ = *dict++;
                if (dst > dst_max) {
                    fprintf(stderr, "WARNING: Dictionary overflow for bytecode 0x06 (%d bytes)\n", i);
                    break;
                }
            }
            break;
        case 0x07:  // 1-byte code + 1 byte from length table
            // Copy l + 14 bytes from the source where l is provided by the (separate) length table
            for (int i = *len++ + 14; i > 0; i--) {
                *dst++ = *dict++;
                if (dst > dst_max) {
                    fprintf(stderr, "WARNING: Dictionary overflow for bytecode 0x07 (%d bytes)\n", i);
                    break;
                }
            }
            break;
        }
    }

    free(code_table);
    return dec_length;
}

// "Compress" a payload
static uint32_t glaze(uint8_t* src, uint32_t src_size, uint8_t** dst)
{
    // Now, there is no way in hell I'm going to craft a bona fide LZ compressor when
    // I have a strong suspicion that this Glaze format that Gust uses comes from a
    // known public compression algorithm, that we simply haven't identified yet.
    // Considering that we have a length table, allowing us to copy ~256 bytes of
    // literals with a single bytecode, we're going to take a massive shortcut by:
    // - Copying all our decompressed data, as is, to the dictionary table
    // - Creating a length table for as many 256-byte blocks we need
    // - Creating a bytecode table, made of only 0x07's, so that only straight block
    //   copies from the dictionary are enacted.
    // Of course, this means the resulting file won't be compressed in the slightest.
    // But we don't really care about that for modding, do we?...

    // Because we're exclusively working with blocks of [14-270] bytes for our
    // "compression" shortcut, we can't handle files that are smaller than 14 bytes...
    if (src_size < 14) {
        fprintf(stderr, "ERROR: Cannot 'compress' files that are smaller than 14 bytes)\n");
        return 0;
    }

    bool short_last_block = (src_size % 256 <= 14);
    uint32_t num_blocks = ((src_size + 255) / 256);
    if (short_last_block)
        num_blocks--;
    // Each block translates to a 5-bit bitstream code (00111b) that yields bytecode 0x07
    uint32_t bitstream_size = ((5 * num_blocks) + 7) / 8;
    // A Glaze compressed file is structured as follows:
    // [decompressed_size] [bistream_size] [bytecode_size] <...bitstream...>
    // [dictionary_size] <...dictionary...> [length_table_size] <...length_table...>
    uint32_t compressed_size = 3 * sizeof(uint32_t) + bitstream_size + sizeof(uint32_t) + src_size + sizeof(uint32_t) + num_blocks;
    *dst = malloc(compressed_size);
    if (*dst == NULL)
        return 0;
    uint8_t* pos = *dst;
    setdata32(pos, src_size);
    pos = &pos[sizeof(uint32_t)];
    // The bitstream size includes the bytecode size field
    setdata32(pos, bitstream_size + sizeof(uint32_t));
    pos = &pos[sizeof(uint32_t)];
    // The bytecode size is our number of blocks
    setdata32(pos, num_blocks);
    pos = &pos[sizeof(uint32_t)];

    // Our bitstream data repeats every 5 bytes, which we use to our advantage
    for (uint32_t i = 0; i < bitstream_size; i += 5) {
        pos[i] = 0x39;
        pos[i + 1] = 0xce;
        pos[i + 2] = 0x73;
        pos[i + 3] = 0x9c;
        pos[i + 4] = 0xe7;
    }
    // Zero the overflow bitstream data just in case
    uint32_t nb_stream_bits_in_last_byte = (5 * num_blocks) % 8;
    if (nb_stream_bits_in_last_byte != 0)
        pos[bitstream_size - 1] &= 0xff << (8 - nb_stream_bits_in_last_byte);

    // Now copy the "dictionary" which is just a verbatim copy of our input
    pos = &pos[bitstream_size];
    setdata32(pos, src_size);
    pos = &pos[sizeof(uint32_t)];
    memcpy(pos, src, src_size);

    // Finally we add our length table
    pos = &pos[src_size];
    setdata32(pos, num_blocks);
    pos = &pos[sizeof(uint32_t)];
    memset(pos, 256 - 14, num_blocks - 1);
    pos = &pos[num_blocks - 1];
    // Our last block can be 14 to 270 bytes in length (with the size offset by 14).
    if (short_last_block)
        *pos = (256 - 14) + (src_size % 256);
    else
        *pos = (src_size % 256) - 14;

    return compressed_size;
}

/*
 * Checksum algorithms
 */
#define ADLER32_MOD 65521
static uint32_t adler32(const uint8_t* data, size_t size)
{
    uint32_t a = 1;
    uint32_t b = 0;

    for (size_t i = 0; i < size; i++) {
        a = (a + data[i]) % ADLER32_MOD;
        b = (b + a) % ADLER32_MOD;
    }

    return (b << 16) | a;
}

static uint32_t checksum_sub(uint8_t* buf, uint32_t buf_size)
{
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < (buf_size & ~3); i += sizeof(uint32_t))
        checksum -= getdata32(&buf[i]);
    return checksum;
}

static uint32_t checksum_xor(uint8_t* buf, uint32_t buf_size)
{
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < (buf_size & ~3); i += sizeof(uint32_t))
        checksum ^= ~getdata32(&buf[i]);
    return checksum;
}

static bool scramble(uint8_t* payload, uint32_t payload_size, char* path, seed_data* seeds,
                     uint32_t working_size, uint32_t version)
{
    bool r = false;
    uint32_t adler_sum, checksum[3] = { 0, 0, 0 };

    // Align the size (plus an extra byte for the end marker) to 16-bytes
    uint32_t main_payload_size = (payload_size + 1 + 0xf) & ~0xf;
    uint8_t* buf = calloc((size_t)main_payload_size + E_HEADER_SIZE + E_FOOTER_SIZE, 1);
    if (buf == NULL)
        return false;
    uint8_t* main_payload = &buf[E_HEADER_SIZE];
    memcpy(main_payload, payload, payload_size);
    adler_sum = adler32(payload, payload_size);

    // Optionally scramble the beginning of the file
    if (version == 2) {
        init_random(adler_sum, seeds->main[2]);
        if (!bit_scrambler(main_payload, min(payload_size, 0x800), 0x80, false))
            goto out;
    }

    // Compute the checksums
    checksum[0] = checksum_sub(main_payload, payload_size);
    checksum[1] = checksum_xor(main_payload, payload_size);
    switch (version) {
    case 2:
#if !defined(VALIDATE_CHECKSUM)
        checksum[2] = adler_sum;
#else
        checksum[2] = VALIDATE_CHECKSUM;
#endif
        break;
    case 3:
        checksum[2] = seeds->main[0];
        break;
    default:
        goto out;
    }

    // Write the checksums
    setdata32(&main_payload[(size_t)main_payload_size + 4], checksum[0]);
    setdata32(&main_payload[(size_t)main_payload_size + 8], checksum[1]);
    setdata32(&main_payload[(size_t)main_payload_size + 12], checksum[2]);

    // Call the main scrambler
    init_random(checksum[2], seeds->table[0]);
    if (!rotating_scrambler(main_payload, payload_size, seeds))
        goto out;

    // Add the end of payload marker
    main_payload[payload_size] = 0xff;

    // From now on, we'll scramble the footer as well
    main_payload_size += E_FOOTER_SIZE;

    // Call first scrambler
    init_random(0, seeds->main[1]);
    if (!fenced_scrambler(main_payload, main_payload_size, seeds->fence, false, (version == 3)))
        goto out;

    // Apply optional extra scrambling to the end of the file
    if (version == 2) {
        init_random(0, seeds->main[0]);
        uint8_t* chunk = &main_payload[main_payload_size - min(main_payload_size, 0x800)];
        if (!bit_scrambler(chunk, min(main_payload_size, 0x800), 0x100, false))
            goto out;
    }

    // Populate the header data
    setdata32(buf, version);
    setdata32(&buf[4], working_size);

    if (!write_file(buf, main_payload_size + E_HEADER_SIZE, path, true))
        goto out;

    r = true;

out:
    free(buf);
    return r;
}

static uint32_t unscramble(uint8_t* payload, uint32_t payload_size, seed_data* seeds,
                           uint32_t* working_size, uint32_t expected_version)
{
    uint32_t version = getbe32(payload);
    if ((version & 0x00ffffff) == 0) {
        version >>= 24;
        is_big_endian = false;
    }
    if ((version != 2) && (version != 3)) {
        fprintf(stderr, "ERROR: Unsupported encoding version: 0x%08x\n", version);
        return 0;
    }
    if (version != expected_version) {
        fprintf(stderr, "WARNING: Expected scrambler v%d file but got scrambler v%d\n",
            expected_version, version);
    }
    *working_size = getdata32(&payload[4]);
    if ((*working_size == 0) || (*working_size > 256 * MB)) {
        fprintf(stderr, "ERROR: Unexpected working size: 0x%08x\n", *working_size);
        return 0;
    }
    payload = &payload[E_HEADER_SIZE];
    payload_size -= E_HEADER_SIZE;

    // Revert the optional bit scrambling applied to the end of the file
    if (version == 2) {
        uint8_t* chunk = &payload[payload_size - min(payload_size, 0x800)];
        init_random(0, seeds->main[0]);
        if (!bit_scrambler(chunk, min(payload_size, 0x800), 0x100, true))
            return 0;
    }

    // Now call the fenced scrambler on the whole payload
    init_random(0, seeds->main[1]);
    if (!fenced_scrambler(payload, payload_size, seeds->fence, true, (version == 3)))
        return 0;

    // Read the descrambled checksums footer (16 bytes)
    uint32_t* footer = (uint32_t*)&payload[payload_size - E_FOOTER_SIZE];
    payload_size -= E_FOOTER_SIZE;
    if ((getdata32(footer) != 0) && (getdata32(footer) != 0x000000ff) && (getdata32(footer) != 0xff000000)) {
        fprintf(stderr, "ERROR: Unexpected footer value: 0x%08x\n", getdata32(footer));
        return 0;
    }
    // The 3rd checksum is probably leftover from the compression algorithm used
#if defined(VALIDATE_CHECKSUM)
    printf("3rd checksum = 0x%08x\n", getdata32(&footer[3]));
#endif
    uint32_t checksum[3] = { getdata32(&footer[1]), getdata32(&footer[2]), getdata32(&footer[3]) };

    // Look for the bitstream_end marker and adjust our size
    for (; (payload_size > 0) && (payload[payload_size] != 0xff); payload_size--);
    if ((payload_size < sizeof(uint32_t)) || (payload[payload_size] != 0xff)) {
        fprintf(stderr, "ERROR: End marker was not found\n");
        return 0;
    }
    payload[payload_size] = 0x00;

    if ((version == 3) && (checksum[2] != seeds->main[0])) {
        fprintf(stderr, "ERROR: Unexpected end seed (wanted: 0x%08x, got: 0x%08x)\n",
            seeds->main[0], checksum[2]);
        return 0;
    }

    // Now call the rotating scrambler on the actual payload
    init_random(checksum[2], seeds->table[0]);
    if (!rotating_scrambler(payload, payload_size, seeds))
        return 0;

    // Validate the checksums
    checksum[0] -= checksum_sub(payload, payload_size);
    checksum[1] ^= checksum_xor(payload, payload_size);
    if ((checksum[0] != 0) || (checksum[1] != 0)) {
        fprintf(stderr, "ERROR: Descrambler checksum mismatch\n");
        return 0;
    }

    // Zero 16 bytes from the end marker position
    for (uint32_t i = 0; i < E_FOOTER_SIZE; i++)
        payload[payload_size + i] = 0;

    // Revert the optional bit scrambling applied to the start of the file
    if (version == 2) {
        init_random(checksum[2], seeds->main[2]);
        if (!bit_scrambler(payload, min(payload_size, 0x800), 0x80, true))
            return 0;
    }

    return payload_size;
}

// Returns the truncated integer square root of y using the Babylonian
// iterative approximation method, derived from Newton's method.
// This public domain function was written by George Gesslein II.
static inline uint32_t lsqrt(uint32_t y)
{
    uint32_t x_old, x_new, testy;
    int i, nbits;

    if (y == 0)
        return 0;

    // Select a good starting value using binary logarithms
    nbits = sizeof(y) * 8;
    for (i = 4, testy = 16; ; i += 2, testy <<= 2) {
        if (i >= nbits || y <= testy) {
            x_old = (1 << (i / 2));	/* x_old = sqrt(testy) */
            break;
        }
    }
    // x_old >= sqrt(y)
    // Use the Babylonian method to arrive at the integer square root
    for (;;) {
        x_new = (y / x_old + x_old) / 2;
        if (x_old <= x_new)
            break;
        x_old = x_new;
    }
    return x_old;
}

// Returns true if 'n' is a prime number recorded in the table
static inline int is_prime (uint32_t n)
{
    uint16_t bit = (uint16_t)n & 0x07;
    return prime_list[n >> 3] & (1 << bit);
}

// Record 'n' as a prime number in the table
static inline void set_prime (uint32_t n)
{
    uint16_t bit = (uint16_t)n & 0x07;
    prime_list[n >> 3] |= (1 << bit);
}

// Check whether 'n' is a prime number.
static bool check_for_prime(uint32_t n)
{
    uint32_t i = 0;
    uint8_t* p;
    uint32_t last_value;
    bool small_n = ((n & 0xffff0000) == 0);

    // We can stop when we have checked all prime numbers below sqrt(n)
    last_value = lsqrt(n);

    // Scan the bitmap of prime numbers and divide 'n' by the corresponding
    // prime to see if it's a multiple of it.
    p = prime_list;
    do {
        uint8_t val = *p++;
        if (val) {
            uint16_t q = (uint16_t)i;
            for (uint16_t j = 1; val && j <= 0x80; j <<= 1, q++) {
                if (val & j) {
                    val &= ~j;
                    // Use 16-bit division if 'n' is small enough.
                    if (small_n) {
                        uint16_t r = (uint16_t)n % (uint16_t)q;
                        if (r == 0)
                            return false;
                    } else {
                        uint32_t r = n % q;
                        if (r == 0)
                            return false;
                    }
                }
            }
        }
        i += 8;
    } while (i < last_value);
    return true;
}

static void compute_prime_list(uint32_t max_value)
{
    uint32_t i;

    prime_list = calloc((max_value + 7) / 8, 1);
    for (i = 2; i <= max_value; i++) {
        if (check_for_prime(i)) {
            set_prime(i);
        }
    }
    set_prime(0);
    set_prime(1);
}

int main_utf8(int argc, char** argv)
{
    seed_data seeds;
    char path[PATH_MAX];
    uint32_t src_size, dst_size;
    uint8_t *src = NULL, *dst = NULL;
    int r = -1;
    const char* app_name = _appname(argv[0]);
    const char* dir_name = _dirname(argv[0]);
    if ((argc < 2) || ((argc == 3) && (*argv[1] != '-'))) {
        printf("%s %s (c) 2019-2021 VitaSmith\n\nUsage: %s [-GAME_ID] <file>\n\n"
            "Encode or decode a Gust .e file.\n\n"
            "If GAME_ID is not provided, then the default game ID from '%s.json' is used.\n"
            "Note: A backup (.bak) of the original is automatically created, when the target\n"
            "is being overwritten for the first time.\n",
            app_name, GUST_TOOLS_VERSION_STR, app_name, app_name);
        return 0;
    }

    // Populate the descrambling seeds from the JSON file
    snprintf(path, sizeof(path), "%s%c%s.json", dir_name, PATH_SEP, app_name);
    JSON_Value* json = json_parse_file_with_comments(path);
    if (json == NULL) {
        // Fall back to default directory if dir_name didn't work
        snprintf(path, sizeof(path), "%s.json", app_name);
        json = json_parse_file_with_comments(path);
        if (json == NULL) {
            fprintf(stderr, "ERROR: Can't parse JSON data from '%s'\n", path);
            goto out;
        }
    }
    const char* seeds_id = (argc == 3) ? &argv[1][1] : json_object_get_string(json_object(json), "seeds_id");
    JSON_Array* seeds_array = json_object_get_array(json_object(json), "seeds");
    JSON_Object* seeds_entry = NULL;
    for (size_t i = 0; i < json_array_get_count(seeds_array); i++) {
        seeds_entry = json_array_get_object(seeds_array, i);
        if (strcmp(seeds_id, json_object_get_string(seeds_entry, "id")) == 0)
            break;
        seeds_entry = NULL;
    }
    if (seeds_entry == NULL) {
        fprintf(stderr, "ERROR: Can't find the seeds for \"%s\" in '%s'\n", seeds_id, path);
        json_value_free(json);
        goto out;
    }

    printf("Using the scrambling seeds for %s", json_object_get_string(seeds_entry, "name"));
    if (argc < 3)
        printf(" (edit '%s' to change)\n", path);
    else
        printf("\n");

    // Get the scrambler version to use
    uint32_t version = json_object_get_uint32(seeds_entry, "version");
    if (version == 3)
        is_big_endian = false;
    uint32_t max_seed_value = 0;
    for (size_t i = 0; i < array_size(seeds.main); i++) {
        seeds.main[i] = (uint32_t)json_array_get_number(json_object_get_array(seeds_entry, "main"), i);
        if (seeds.main[i] > max_seed_value)
            max_seed_value = seeds.main[i];
        seeds.table[i] = (uint32_t)json_array_get_number(json_object_get_array(seeds_entry, "table"), i);
        if (seeds.table[i] > max_seed_value)
            max_seed_value = seeds.table[i];
        seeds.length[i] = (uint32_t)json_array_get_number(json_object_get_array(seeds_entry, "length"), i);
    }
    seeds.fence = (uint16_t)json_object_get_number(seeds_entry, "fence");
    bool validate_primes = json_object_get_boolean(json_object(json), "validate_primes");
    json_value_free(json);

    // Validate the primes. You can disable this check by setting validate_primes to false in JSON.
    if (validate_primes) {
        compute_prime_list(max_seed_value);
        for (size_t i = 0; i < array_size(seeds.main); i++) {
            if (!is_prime(seeds.main[i])) {
                printf("ERROR: main[%d] (0x%04x) is not prime!\n", (uint32_t)i, seeds.main[i]);
                goto out;
            }
            if (!is_prime(seeds.table[i])) {
                printf("ERROR: table[%d] (0x%04x) is not prime!\n", (uint32_t)i, seeds.table[i]);
                goto out;
            }
            if (!is_prime(seeds.length[i])) {
                printf("ERROR: length[%d] (0x%02x) is not prime!\n", (uint32_t)i, seeds.length[i]);
                goto out;
            }
            if (!is_prime(seeds.fence)) {
                printf("ERROR: fence (0x%04x) is not prime!\n", seeds.fence);
                goto out;
            }
        }
    }

    // Read the source file
    src_size = read_file(argv[argc - 1], &src);
    if (src_size == UINT32_MAX)
        goto out;

    char* e_pos = strstr(argv[argc - 1], ".e");
    if (e_pos == NULL) {
        printf("Encoding '%s'...\n", _basename(argv[argc - 1]));
        // Compress and scramble a file
#if defined(USE_GLAZED)
        dst = malloc(src_size);
        memcpy(dst, src, src_size);
        dst_size = src_size;
#else
        dst_size = glaze(src, src_size, &dst);
        if (dst_size == 0)
            goto out;
#endif

#if defined(CREATE_EXTRA_FILES)
        snprintf(path, sizeof(path), "%s.glaze", basename(argv[argc - 1]));
        write_file(dst, dst_size, path, false);
#endif

#if defined(VALIDATE_CHECKSUM)
        printf("UnGlaze: 0x%08x, src_size = 0x%08x\n", unglaze(dst, dst_size, src, src_size), src_size);
#endif

        // Scramble the Glaze compressed file
        // IMPORTANT: The Atelier executables allocate a working buffer of size 'working_size'
        // for the decoding operation which must be at least the size of the uncompressed data
        // or the size of the compressed stream plus the size of the bytecode table, whichever
        // is largest (because this buffer will be zeroed for the size of the compressed stream
        // plus the size of the bytecode table once decompression is complete).
        uint32_t working_size = max(src_size, dst_size + getdata32(&dst[2 * sizeof(uint32_t)]));
        snprintf(path, sizeof(path), "%s.e", argv[argc - 1]);
        if (!scramble(dst, dst_size, path, &seeds, working_size, version))
            goto out;

        r = 0;
    } else {
        printf("Decoding '%s'...\n", _basename(argv[argc - 1]));
        // Decode a file
        if (((src_size % 4) != 0) || (src_size <= E_HEADER_SIZE + E_FOOTER_SIZE)) {
            fprintf(stderr, "ERROR: Invalid file size\n");
            goto out;
        }

        // Descramble the data
        uint32_t working_size = 0;
        uint32_t payload_size = unscramble(src, src_size, &seeds, &working_size, version);
        if ((payload_size == 0) || (working_size == 0))
            goto out;

#if defined(CREATE_EXTRA_FILES)
        snprintf(path, sizeof(path), "%s.glaze", argv[argc - 1]);
        write_file(&src[E_HEADER_SIZE], payload_size, path, false);
#endif

#if defined(VALIDATE_CHECKSUM)
        // "We can rebuild (it), we have the technology."
        snprintf(path, sizeof(path), "%s.rebuilt", argv[argc - 1]);
        scramble(&src[E_HEADER_SIZE], payload_size, path, &seeds, working_size, version);
#endif

        // Uncompress descrambled data
        dst = malloc(working_size);
        if (dst == NULL)
            goto out;
        dst_size = unglaze(&src[E_HEADER_SIZE], payload_size, dst, working_size);
        if (dst_size == 0)
            goto out;

        *e_pos = 0;
        if (!write_file(dst, dst_size, argv[argc - 1], true))
            goto out;
        r = 0;
    }

    // What a wild ride it has been to get there...
    // Thank you Gust, for making the cracking of your "encryption"
    // even more interesting than playing your games! :)))

out:
    free(prime_list);
    free(dst);
    free(src);

    if (r != 0) {
        fflush(stdin);
        printf("\nPress any key to continue...");
        (void)getchar();
    }

    return r;
}

CALL_MAIN
