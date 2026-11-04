/* SPDX-License-Identifier: Apache-2.0 */
/* Compile these assertions with the actual pinned Arm GCC and LLVM headers.
 * GCC 10 compatibility must preserve trait semantics, not force them true. */
#include <type_traits>
namespace {
struct Throwing
{
  Throwing() noexcept(false);
  Throwing(const Throwing &) noexcept(false);
  Throwing &operator=(const Throwing &) noexcept(false);
};
struct Deleted { Deleted &operator=(const Deleted &)=delete; };
static_assert(std::is_nothrow_default_constructible<int>::value,"int default");
static_assert(std::is_nothrow_copy_constructible<int>::value,"int copy");
static_assert(std::is_nothrow_move_constructible<int>::value,"int move");
static_assert(std::is_nothrow_copy_assignable<int>::value,"int copy assignment");
static_assert(std::is_nothrow_move_assignable<int>::value,"int move assignment");
static_assert(std::is_nothrow_assignable<int &,int>::value,"lvalue assignment");
static_assert(!std::is_nothrow_assignable<int,int>::value,"rvalue is not assignable");
static_assert(!std::is_nothrow_default_constructible<void>::value,"void default");
static_assert(!std::is_nothrow_copy_assignable<const int>::value,"const assignment");
static_assert(!std::is_nothrow_default_constructible<Throwing>::value,"throwing constructor");
static_assert(!std::is_nothrow_copy_assignable<Throwing>::value,"throwing assignment");
static_assert(!std::is_nothrow_move_assignable<Throwing>::value,"throwing copy fallback");
static_assert(!std::is_nothrow_copy_assignable<Deleted>::value,"deleted assignment");
}
