#include "num.h"

#include "str.h"
#include <math.h>


// Max length of a string representation of a number
#define MU_NUMLEN 12


// Returns true if both variables are equal
bool num_equals(num_t a, num_t b) {
    return a == b;
}

// Returns a hash for each number
// For positive integers this is equivalent to the number
hash_t num_hash(num_t n) {
    mu_t ipart, fpart;

    // This magic number is the value to puts a number's mantissa
    // directly in the integer range. After these operations,
    // ipart and fpart will contain the integer and fractional
    // components of the original number.
    ipart.num = n + 12582912.0f;
    fpart.num = n - (ipart.num - 12582912.0f);

    // The int component forms the core of the hash so integers
    // remain linear for table lookups. The fractional component 
    // is also used keep the hash sane for non-integer values.
    // TODO use the bits where the exponent was for something
    return 0x807fffff & (ipart.bits ^ fpart.bits);
}

// Parses a string and returns a number
num_t num_parse(const data_t **off, const data_t *end) {
    const data_t *str = *off;
    num_t res = 0;

    num_t scale, sign;

    struct base {
        num_t radix;
        num_t exp;
        data_t expc;
        data_t expC;
    } base = { 10.0, 10.0, 'e', 'E' };


    // determine the base
    if (end-str > 2 && *str == '0') {
        switch (str[1]) {
            case 'b': case 'B':
                base = (struct base){ 2.0, 2.0, 'p', 'P' };
                str += 2;
                break;

            case 'o': case 'O':
                base = (struct base){ 8.0, 2.0, 'p', 'P' };
                str += 2;
                break;

            case 'x': case 'X':
                base = (struct base){ 16.0, 2.0, 'p', 'P' };
                str += 2;
                break;
        }
    }

    // determine the integer component
    while (str < end) {
        int n = num_val(*str);

        if (n >= base.radix) {
            if (*str == '.')
                goto fraction;
            else if (*str == base.expc || *str == base.expC)
                goto exp;
            else
                goto done;
        }

        str++;
        res *= base.radix;
        res += (num_t)n;
    }

    goto done;

fraction:   // determine fraction component
    scale = 1.0;

    while (str < end) {
        int n = num_val(*str);

        if (n >= base.radix) {
            if (*str == base.expc || *str == base.expC)
                goto exp;
            else
                goto done;
        }

        str++;
        scale /= base.radix;
        res += scale * (num_t)n;
    }

    goto done;

exp:        // determine exponent component
    scale = 0.0;
    sign = 1.0;

    if (end-str > 1) {
        if (*str == '+') {
            str++;
        } else if (*str == '-') {
            sign = -1.0;
            str++;
        }
    }

    while (str < end) {
        int n = num_val(*str);

        if (n >= base.radix) {
            res *= (num_t)pow(base.radix, sign*scale);
            goto done;
        }

        str++;
        scale *= base.radix;
        scale += (num_t)n;
    }

done:       // return the result
    *off = str;

    return res;
}


// Obtains a string representation of a number
str_t *num_repr(num_t n) {
    if (n == 0) {
        return getstr(mcstr("0"));
    } else if (isnan(n)) {
        return getstr(mcstr("nan"));
    } else if (isinf(n)) {
        return getstr(n > 0.0 ? mcstr("inf") : mcstr("-inf"));
    } else {
        mstr_t *m = mstr_create(MU_NUMLEN);
        data_t *out = m->data;

        if (n < 0.0) {
            n = -n;
            *out++ = '-';
        }

        num_t exp = floor(log10(n));
        num_t digit = pow(10.0, exp);
        bool isexp = (exp > MU_NUMLEN-2 || exp < -(MU_NUMLEN-3));

        if (isexp) {
            n /= digit;
            digit = 1.0;
        } else if (digit < 1.0) {
            digit = 1.0;
        }


        int len = isexp ? MU_NUMLEN-6 : MU_NUMLEN-1;

        for (; len >= 0; len--) {
            if (n <= 0.0 && digit < 1.0)
                break;

            if (digit < 0.5 && digit > 0.05)
                *out++ = '.';

            num_t d = floor(n / digit);
            *out++ = num_ascii((int_t)d);

            n -= d * digit;
            digit /= 10.0f;
        }


        if (isexp) {
            *out++ = 'e';

            if (exp < 0) {
                exp = -exp;
                *out++ = '-';
            }

            if (exp > 100)
                *out++ = num_ascii(((int_t)exp) / 100);

            // exp will always be greater than 10 here
            *out++ = num_ascii(((int_t)exp / 10) % 10);
            *out++ = num_ascii(((int_t)exp / 1) % 10);
        }

        m->len = out - m->data;
        return str_intern(m);
    }
}

