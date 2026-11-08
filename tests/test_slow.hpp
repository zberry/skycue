#pragma once

// Upper-bound waits in the tests scale with SKYCUE_TEST_SLOW (see
// net/tests/slow.hpp, the one definition: the net library's tests must not
// include anything from outside net/, so skycued's tests include it).
#include "../net/tests/slow.hpp"
