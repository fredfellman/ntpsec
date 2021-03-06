#include "config.h"

#include "ntp_stdlib.h"
#include "ntp_fp.h"

#include "unity.h"
#include "unity_fixture.h"

#include <float.h>
#include <math.h>

TEST_GROUP(lfpfunc);

TEST_SETUP(lfpfunc) {}

TEST_TEAR_DOWN(lfpfunc) {}

//----------------------------------------------------------------------
// reference comparison
// This is implemented as a full signed MP-subtract in 3 limbs, where
// the operands are zero or sign extended before the subtraction is
// executed.
//----------------------------------------------------------------------

// maybe rename it to lf_cmp_work
static int cmp_work(uint32_t a[3], uint32_t b[3])
{
	uint32_t cy, idx, tmp;
	for (cy = idx = 0; idx < 3; ++idx) {
		tmp = a[idx]; cy  = (a[idx] -=   cy  ) > tmp;
		tmp = a[idx]; cy |= (a[idx] -= b[idx]) > tmp;
	}
	if (a[2])
		return -1;
	return a[0] || a[1];
}

static int l_fp_scmp(const l_fp first, const l_fp second)
{
	uint32_t a[3], b[3];

	const l_fp op1 = first;
	const l_fp op2 = second;

	a[0] = lfpfrac(op1); a[1] = lfpuint(op1); a[2] = 0;
	b[0] = lfpfrac(op2); b[1] = lfpuint(op2); b[2] = 0;

	a[2] -= (lfpsint(op1) < 0);
	b[2] -= (lfpsint(op2) < 0);

	return cmp_work(a,b);
}

static int l_fp_ucmp(const l_fp first, l_fp second)
{
	uint32_t a[3], b[3];
	const l_fp op1 = first; 
	const l_fp op2 = second;

	a[0] = lfpfrac(op1); a[1] = lfpuint(op1); a[2] = 0;
	b[0] = lfpfrac(op2); b[1] = lfpuint(op2); b[2] = 0;

	return cmp_work(a,b);
}

//----------------------------------------------------------------------
// imlementation of the LFP stuff
// This should be easy enough...
//----------------------------------------------------------------------

static l_fp l_fp_negate(const l_fp first)
{
	l_fp temp = first;
	L_NEG(temp);

	return temp;
}

static l_fp l_fp_abs(const l_fp first)
{
	l_fp temp = first;
	if (L_ISNEG(temp))
		L_NEG(temp);
	return temp;
}

static int l_fp_signum(const l_fp first)
{
	if (lfpuint(first) & 0x80000000u)
		return -1;
	return (lfpuint(first) || lfpfrac(first));
}

static void l_fp_swap(l_fp * first, l_fp *second)
{
	l_fp temp = *second;

	*second = *first;
	*first = temp;

	return;
}

//----------------------------------------------------------------------
// testing the relational macros works better with proper predicate
// formatting functions; it slows down the tests a bit, but makes for
// readable failure messages.
//----------------------------------------------------------------------


static bool l_isgt (const l_fp first, const l_fp second)
{
	return L_ISGT(first, second);
}

static bool l_isgtu(const l_fp first, const l_fp second)
{
	return L_ISGTU(first, second);
}

//----------------------------------------------------------------------
// test data table for add/sub and compare
//----------------------------------------------------------------------


static const l_fp_w addsub_tab[][3] = {
	// trivial identity:
	{{0 ,0         }, { 0,0         }, { 0,0}},
	// with carry from fraction and sign change:
	{{-1,0x80000000}, { 0,0x80000000}, { 0,0}},
	// without carry from fraction
	{{ 1,0x40000000}, { 1,0x40000000}, { 2,0x80000000}},
	// with carry from fraction:
	{{ 1,0xC0000000}, { 1,0xC0000000}, { 3,0x80000000}},
	// with carry from fraction and sign change:
	{{0x7FFFFFFF, 0x7FFFFFFF}, {0x7FFFFFFF,0x7FFFFFFF}, {0xFFFFFFFE,0xFFFFFFFE}},
	// two tests w/o carry (used for l_fp<-->double):
	{{0x55555555,0xAAAAAAAA}, {0x11111111,0x11111111}, {0x66666666,0xBBBBBBBB}},
	{{0x55555555,0x55555555}, {0x11111111,0x11111111}, {0x66666666,0x66666666}},
	// wide-range test, triggers compare trouble
	{{0x80000000,0x00000001}, {0xFFFFFFFF,0xFFFFFFFE}, {0x7FFFFFFF,0xFFFFFFFF}}
};
static const size_t addsub_cnt = (sizeof(addsub_tab)/sizeof(addsub_tab[0]));
static const size_t addsub_tot = (sizeof(addsub_tab)/sizeof(addsub_tab[0][0]));



//----------------------------------------------------------------------
// epsilon estimation for the precision of a conversion double --> l_fp
//
// The error estimation limit is as follows:
//  * The 'l_fp' fixed point fraction has 32 bits precision, so we allow
//    for the LSB to toggle by clamping the epsilon to be at least 2^(-31)
//
//  * The double mantissa has a precsion 54 bits, so the other minimum is
//    dval * (2^(-53))
//
//  The maximum of those two boundaries is used for the check.
//
// Note: once there are more than 54 bits between the highest and lowest
// '1'-bit of the l_fp value, the roundtrip *will* create truncation
// errors. This is an inherent property caused by the 54-bit mantissa of
// the 'double' type.
double
eps(double d)
{
	return fmax(ldexp(1.0, -31), ldexp(fabs(d), -53));
}

//----------------------------------------------------------------------
// test extractor functions
//----------------------------------------------------------------------

TEST(lfpfunc, Extraction) {
    const uint32_t hi = 0xFFEEDDBB;
    const uint32_t lo = 0x66554433;
    l_fp lfp = lfpinit(hi, lo);
    TEST_ASSERT_EQUAL(lfpuint(lfp), hi);
    TEST_ASSERT_EQUAL(lfpfrac(lfp), lo);
    TEST_ASSERT_EQUAL(lfpsint(lfp), -1122885);
    l_fp bumpable = lfpinit(333, 444);
    bumplfpuint(bumpable, 1);
    TEST_ASSERT_EQUAL(lfpuint(bumpable), 334);
    TEST_ASSERT_EQUAL(lfpfrac(bumpable), 444);
}

//----------------------------------------------------------------------
// test negation
//----------------------------------------------------------------------

TEST(lfpfunc, Negation) {
	size_t idx = 0;

	for (idx = 0; idx < addsub_cnt; ++idx) {
		l_fp op1 = lfpinit(addsub_tab[idx][0].l_ui, addsub_tab[idx][0].l_uf);
		l_fp op2 = l_fp_negate(op1);
		l_fp sum = op1 + op2;

		TEST_ASSERT_EQUAL(0, sum);
	}
	return;
}



//----------------------------------------------------------------------
// test absolute value
//----------------------------------------------------------------------
TEST(lfpfunc, Absolute) {
	size_t idx = 0;

	for (idx = 0; idx < addsub_cnt; ++idx) {
		l_fp op1 = lfpinit(addsub_tab[idx][0].l_ui, addsub_tab[idx][0].l_uf);
		l_fp op2 = l_fp_abs(op1);

		TEST_ASSERT_TRUE(l_fp_signum(op2) >= 0);

		if (l_fp_signum(op1) >= 0)
			op1 = op1 - op2;
		else
			op1 = op1 + op2;

		TEST_ASSERT_EQUAL(0, op1);
	}

	// There is one special case we have to check: the minimum
	// value cannot be negated, or, to be more precise, the
	// negation reproduces the original pattern.
	l_fp minVal = lfpinit(0x80000000, 0x00000000);
	l_fp minAbs = l_fp_abs(minVal);
	TEST_ASSERT_EQUAL(-1, l_fp_signum(minVal));

	TEST_ASSERT_EQUAL(minVal, minAbs);

	return;
}


//----------------------------------------------------------------------
// fp -> double -> fp roundtrip test
//----------------------------------------------------------------------
TEST(lfpfunc, FDF_RoundTrip) {
	size_t idx = 0;

	// since a l_fp has 64 bits in its mantissa and a double has
	// only 54 bits available (including the hidden '1') we have to
	// make a few concessions on the roundtrip precision. The 'eps()'
	// function makes an educated guess about the available precision
	// and checks the difference in the two 'l_fp' values against
	// that limit.

	for (idx = 0; idx < addsub_cnt; ++idx) {
		l_fp op1 = lfpinit(addsub_tab[idx][0].l_ui, addsub_tab[idx][0].l_uf);
		double op2 = lfptod(op1);
		l_fp op3 = dtolfp(op2);

		l_fp temp = op1 - op3;
		double d = lfptod(temp);
		TEST_ASSERT_DOUBLE_WITHIN(eps(op2), 0.0, fabs(d));
	}

	return;
}


//----------------------------------------------------------------------
// test the compare stuff
//
// This uses the local compare and checks if the operations using the
// macros in 'ntp_fp.h' produce mathing results.
// ----------------------------------------------------------------------
TEST(lfpfunc, SignedRelOps) {
	const l_fp_w * tv = (&addsub_tab[0][0]);
	size_t lc ;

	for (lc = addsub_tot - 1; lc; --lc, ++tv) {
		l_fp op1 = lfpinit(tv[0].l_ui, tv[0].l_uf);
		l_fp op2 = lfpinit(tv[1].l_ui, tv[1].l_uf);
		int cmp = l_fp_scmp(op1, op2);

		switch (cmp) {
		case -1:
			//printf("op1:%d %d, op2:%d %d\n",lfpfrac(op1),lfpuint(op1),lfpfrac(op2),lfpuint(op2));
			l_fp_swap(&op1, &op2);
			//printf("op1:%d %d, op2:%d %d\n",lfpfrac(op1),lfpuint(op1),lfpfrac(op2),lfpuint(op2));
		case 1:
			TEST_ASSERT_TRUE (l_isgt(op1, op2));
			TEST_ASSERT_FALSE(l_isgt(op2, op1));
			break;
		case 0:
			TEST_ASSERT_FALSE(l_isgt(op1, op2));
			TEST_ASSERT_FALSE(l_isgt(op2, op1));
			break;
		default:
			TEST_FAIL_MESSAGE("unexpected UCMP result: ");
		}
	}

	return;
}

TEST(lfpfunc, UnsignedRelOps) {
	const l_fp_w *tv = (&addsub_tab[0][0]);
	size_t lc;

	for (lc = addsub_tot - 1; lc; --lc, ++tv) {
		l_fp op1 = lfpinit(tv[0].l_ui, tv[0].l_uf);
		l_fp op2 = lfpinit(tv[1].l_ui, tv[1].l_uf);
		int cmp = l_fp_ucmp(op1, op2);

		switch (cmp) {
		case -1:
			//printf("op1:%d %d, op2:%d %d\n",lfpfrac(op1),lfpuint(op1),lfpfrac(op2),lfpuint(op2));
			l_fp_swap(&op1, &op2);
			//printf("op1:%d %d, op2:%d %d\n",lfpfrac(op1),lfpuint(op1),lfpfrac(op2),lfpuint(op2));
		case 1:
			TEST_ASSERT_TRUE (l_isgtu(op1, op2));
			TEST_ASSERT_FALSE(l_isgtu(op2, op1));
			break;
		case 0:
			TEST_ASSERT_FALSE(l_isgtu(op1, op2));
			TEST_ASSERT_FALSE(l_isgtu(op2, op1));
			break;
		default:
			TEST_FAIL_MESSAGE("unexpected UCMP result: ");
		}
	}

	return;
}

/*
*/

//----------------------------------------------------------------------
// that's all folks... but feel free to add things!
//----------------------------------------------------------------------

TEST_GROUP_RUNNER(lfpfunc) {
	RUN_TEST_CASE(lfpfunc, Extraction);
	RUN_TEST_CASE(lfpfunc, Negation);
	RUN_TEST_CASE(lfpfunc, Absolute);
	RUN_TEST_CASE(lfpfunc, FDF_RoundTrip);
	RUN_TEST_CASE(lfpfunc, SignedRelOps);
	RUN_TEST_CASE(lfpfunc, UnsignedRelOps);
}
