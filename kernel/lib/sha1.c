/*
 * *SHA transform algorithm, originally taken from code written by*
 * *Peter Gutmann, and placed in the public domain.*
 * SHA1 routine optimized to do word accesses rather than byte accesses,
 * and to avoid unnecessary copies into the context array.
 *
 * This was based on the git SHA1 implementation.
 * commit:1eb19a12bd2214cdcad5273d472b062a4ba97fa1
 * lib/sha1: use the git implementation of SHA-1
 */

#include <linux/kernel.h>
#include <linux/module.h>
/* #include <linux/cryptohash.h> */
#include <linux/bitops.h>
#include <asm/unaligned.h>

/* *The SHA f()-functions.*  */
/*
 * If you have 32 registers or more, the compiler can (and should)
 * try to change the array[] accesses into registers. However, on
 * machines with less than ~25 registers, that won't really work,
 * and at least gcc will make an unholy mess of it.
 *
 * So to avoid that mess which just slows things down, we force
 * the stores to memory to actually happen (we might be better off
 * with a 'W(t)=(val);asm("":"+m" (W(t))' there instead, as
 * suggested by Artur Skawina - that will also make gcc unable to
 * try to do the silly "optimize away loads" part because it won't
 * see what the value will be).
 *
 * Ben Herrenschmidt reports that on PPC, the C version comes close
 * to the optimized asm with this (ie on PPC you don't want that
 * 'volatile', since there are lots of registers).
 *
 * On ARM we get the best code generation by forcing a full memory barrier
 * between each SHA_ROUND, otherwise gcc happily get wild with spilling and
 * the stack frame size simply explode and performance goes down the drain.
 */

/* #define f1(x,y,z)   (z ^ (x & (y ^ z)))		 * x ? y : z *
#define f2(x,y,z)   (x ^ y ^ z)			 * XOR *
#define f3(x,y,z)   ((x & y) + (z & (x ^ y)))	 * majority * */
#ifdef CONFIG_X86
  #define setW(x, val) (*(volatile __u32 *)&W(x) = (val))
#elif defined(CONFIG_ARM)
  #define setW(x, val) do { W(x) = (val); __asm__("":::"memory"); } while (0)
#else
  #define setW(x, val) (W(x) = (val))
#endif

/* *The SHA Mysterious Constants* */
/* This "rolls" over the 512-bit array */
#define W(x) (array[(x)&15])
/*
#define K1  0x5A827999L			* Rounds  0-19: sqrt(2) * 2^30 *
#define K2  0x6ED9EBA1L			* Rounds 20-39: sqrt(3) * 2^30 *
#define K3  0x8F1BBCDCL			* Rounds 40-59: sqrt(5) * 2^30 *
#define K4  0xCA62C1D6L			* Rounds 60-79: sqrt(10) * 2^30 **/
/*
 * Where do we get the source from? The first 16 iterations get it from
 * the input data, the next mix it from the 512-bit array.
 */
#define SHA_SRC(t) get_unaligned_be32((__u32 *)data + t)
#define SHA_MIX(t) rol32(W(t+13) ^ W(t+8) ^ W(t+2) ^ W(t), 1)

#define SHA_ROUND(t, input, fn, constant, A, B, C, D, E) do { \
	__u32 TEMP = input(t); setW(t, TEMP); \
	E += TEMP + rol32(A,5) + (fn) + (constant); \
	B = ror32(B, 2); } while (0)

#define T_0_15(t, A, B, C, D, E)  SHA_ROUND(t, SHA_SRC, (((C^D)&B)^D) , 0x5a827999, A, B, C, D, E )
#define T_16_19(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, (((C^D)&B)^D) , 0x5a827999, A, B, C, D, E )
#define T_20_39(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, (B^C^D) , 0x6ed9eba1, A, B, C, D, E )
#define T_40_59(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, ((B&C)+(D&(B^C))) , 0x8f1bbcdc, A, B, C, D, E )
#define T_60_79(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, (B^C^D) ,  0xca62c1d6, A, B, C, D, E )

/**
 * sha_transform - single block SHA1 transform
 *
 * @digest: 160 bit digest to update
 * @data:   512 bits of data to hash
 * *@W:      80 words of workspace (see note)*
 * @array:  16 words of workspace (see note)
 *
 * This function generates a SHA1 digest for a single 512-bit block.
 * Be warned, it does not handle padding and message digest, do not
 * confuse it with the full FIPS 180-1 digest algorithm for variable
 * length messages.
 *
 * Note: If the hash is security sensitive, the caller should be sure
 * to clear the workspace. This is left to the caller to avoid
 * unnecessary clears between chained hashing operations.
 */
/*void sha_transform(__u32 *digest, const char *in, __u32 *W) */
void sha_transform(__u32 *digest, const char *data, __u32 *array)
{
/*	__u32 a, b, c, d, e, t, i; */
	__u32 A, B, C, D, E;

/*	for (i = 0; i < 16; i++)
		W[i] = be32_to_cpu(((const __be32 *)in)[i]); */
	A = digest[0];
	B = digest[1];
	C = digest[2];
	D = digest[3];
	E = digest[4];		

/*	for (i = 0; i < 64; i++)
		W[i+16] = rol32(W[i+13] ^ W[i+8] ^ W[i+2] ^ W[i], 1); */
	/* Round 1 - iterations 0-16 take their input from 'data' */
	T_0_15( 0, A, B, C, D, E);
	T_0_15( 1, E, A, B, C, D);
	T_0_15( 2, D, E, A, B, C);
	T_0_15( 3, C, D, E, A, B);
	T_0_15( 4, B, C, D, E, A);
	T_0_15( 5, A, B, C, D, E);
	T_0_15( 6, E, A, B, C, D);
	T_0_15( 7, D, E, A, B, C);
	T_0_15( 8, C, D, E, A, B);
	T_0_15( 9, B, C, D, E, A);
	T_0_15(10, A, B, C, D, E);
	T_0_15(11, E, A, B, C, D);
	T_0_15(12, D, E, A, B, C);
	T_0_15(13, C, D, E, A, B);
	T_0_15(14, B, C, D, E, A);
	T_0_15(15, A, B, C, D, E);		

/*	a = digest[0];
	b = digest[1];
	c = digest[2];
	d = digest[3];
	e = digest[4]; */
	/* Round 1 - tail. Input from 512-bit mixing array */
	T_16_19(16, E, A, B, C, D);
	T_16_19(17, D, E, A, B, C);
	T_16_19(18, C, D, E, A, B);
	T_16_19(19, B, C, D, E, A);
/*	
	for (i = 0; i < 20; i++) {
		t = f1(b, c, d) + K1 + rol32(a, 5) + e + W[i];
		e = d; d = c; c = rol32(b, 30); b = a; a = t;
	} */
	/* Round 2 */
	T_20_39(20, A, B, C, D, E);
	T_20_39(21, E, A, B, C, D);
	T_20_39(22, D, E, A, B, C);
	T_20_39(23, C, D, E, A, B);
	T_20_39(24, B, C, D, E, A);
	T_20_39(25, A, B, C, D, E);
	T_20_39(26, E, A, B, C, D);
	T_20_39(27, D, E, A, B, C);
	T_20_39(28, C, D, E, A, B);
	T_20_39(29, B, C, D, E, A);
	T_20_39(30, A, B, C, D, E);
	T_20_39(31, E, A, B, C, D);
	T_20_39(32, D, E, A, B, C);
	T_20_39(33, C, D, E, A, B);
	T_20_39(34, B, C, D, E, A);
	T_20_39(35, A, B, C, D, E);
	T_20_39(36, E, A, B, C, D);
	T_20_39(37, D, E, A, B, C);
	T_20_39(38, C, D, E, A, B);
	T_20_39(39, B, C, D, E, A);
	
/*	for (; i < 40; i ++) {
		t = f2(b, c, d) + K2 + rol32(a, 5) + e + W[i];
		e = d; d = c; c = rol32(b, 30); b = a; a = t;
	} */
	/* Round 3 */
	T_40_59(40, A, B, C, D, E);
	T_40_59(41, E, A, B, C, D);
	T_40_59(42, D, E, A, B, C);
	T_40_59(43, C, D, E, A, B);
	T_40_59(44, B, C, D, E, A);
	T_40_59(45, A, B, C, D, E);
	T_40_59(46, E, A, B, C, D);
	T_40_59(47, D, E, A, B, C);
	T_40_59(48, C, D, E, A, B);
	T_40_59(49, B, C, D, E, A);
	T_40_59(50, A, B, C, D, E);
	T_40_59(51, E, A, B, C, D);
	T_40_59(52, D, E, A, B, C);
	T_40_59(53, C, D, E, A, B);
	T_40_59(54, B, C, D, E, A);
	T_40_59(55, A, B, C, D, E);
	T_40_59(56, E, A, B, C, D);
	T_40_59(57, D, E, A, B, C);
	T_40_59(58, C, D, E, A, B);
	T_40_59(59, B, C, D, E, A);
	
/*	for (; i < 60; i ++) {
		t = f3(b, c, d) + K3 + rol32(a, 5) + e + W[i];
		e = d; d = c; c = rol32(b, 30); b = a; a = t;
	} */
	/* Round 4 */
	T_60_79(60, A, B, C, D, E);
	T_60_79(61, E, A, B, C, D);
	T_60_79(62, D, E, A, B, C);
	T_60_79(63, C, D, E, A, B);
	T_60_79(64, B, C, D, E, A);
	T_60_79(65, A, B, C, D, E);
	T_60_79(66, E, A, B, C, D);
	T_60_79(67, D, E, A, B, C);
	T_60_79(68, C, D, E, A, B);
	T_60_79(69, B, C, D, E, A);
	T_60_79(70, A, B, C, D, E);
	T_60_79(71, E, A, B, C, D);
	T_60_79(72, D, E, A, B, C);
	T_60_79(73, C, D, E, A, B);
	T_60_79(74, B, C, D, E, A);
	T_60_79(75, A, B, C, D, E);
	T_60_79(76, E, A, B, C, D);
	T_60_79(77, D, E, A, B, C);
	T_60_79(78, C, D, E, A, B);
	T_60_79(79, B, C, D, E, A);
	
/*	for (; i < 80; i ++) {
		t = f2(b, c, d) + K4 + rol32(a, 5) + e + W[i];
		e = d; d = c; c = rol32(b, 30); b = a; a = t;
	}

	digest[0] += a;
	digest[1] += b;
	digest[2] += c;
	digest[3] += d;
	digest[4] += e; */
	digest[0] += A;
	digest[1] += B;
	digest[2] += C;
	digest[3] += D;
	digest[4] += E;	
}
EXPORT_SYMBOL(sha_transform);

/**
 * sha_init - initialize the vectors for a SHA1 digest
 * @buf: vector to initialize
 */
void sha_init(__u32 *buf)
{
	buf[0] = 0x67452301;
	buf[1] = 0xefcdab89;
	buf[2] = 0x98badcfe;
	buf[3] = 0x10325476;
	buf[4] = 0xc3d2e1f0;
}