#include "hr_history.h"
#include "test_util.h"

static void dec(char *s) { hr_url_decode(s); }

static void test_plain_unchanged(void)
{
    TEST_CASE("plain text unchanged");
    char s[] = "MyNetwork";
    dec(s);
    CHECK_STR(s, "MyNetwork");
}

static void test_plus_becomes_space(void)
{
    TEST_CASE("plus becomes space");
    char s[] = "my+home+wifi";
    dec(s);
    CHECK_STR(s, "my home wifi");
}

static void test_percent_hex_decoded(void)
{
    TEST_CASE("percent hex decoded");
    char s[] = "P%40ss%21word";  /* P@ss!word */
    dec(s);
    CHECK_STR(s, "P@ss!word");
}

static void test_mixed_and_ampersand_char(void)
{
    TEST_CASE("mixed plus and percent");
    char s[] = "a+b%26c%3Dd";    /* a b&c=d */
    dec(s);
    CHECK_STR(s, "a b&c=d");
}

static void test_lowercase_hex(void)
{
    TEST_CASE("lowercase hex digits");
    char s[] = "%2f%2F";         /* // */
    dec(s);
    CHECK_STR(s, "//");
}

static void test_malformed_percent_left_alone(void)
{
    TEST_CASE("malformed percent left alone");
    char s[] = "50%off";         /* % not followed by 2 hex digits */
    dec(s);
    CHECK_STR(s, "50%off");

    char t[] = "trailing%";
    dec(t);
    CHECK_STR(t, "trailing%");

    char u[] = "bad%z1";
    dec(u);
    CHECK_STR(u, "bad%z1");
}

int main(void)
{
    test_plain_unchanged();
    test_plus_becomes_space();
    test_percent_hex_decoded();
    test_mixed_and_ampersand_char();
    test_lowercase_hex();
    test_malformed_percent_left_alone();
    return TEST_REPORT();
}
