/*
** $Id: lapi.c,v 2.55.1.5 2008/07/04 18:41:18 roberto Exp $
** Lua API
** See Copyright Notice in lua.h
*/


#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#define lapi_c
#define LUA_CORE

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"


/*
 * 大部分API 都需要先 lua_lock(L)
 *
 * 完成功能后再lua_unlock(L)
 */



const char lua_ident[] =
  "$Lua: " LUA_RELEASE " " LUA_COPYRIGHT " $\n"
  "$Authors: " LUA_AUTHORS " $\n"
  "$URL: www.lua.org $\n";



#define api_checknelems(L, n)	api_check(L, (n) <= (L->top - L->base))

#define api_checkvalidindex(L, i)	api_check(L, (i) != luaO_nilobject)

#define api_incr_top(L)   {api_check(L, L->top < L->ci->top); L->top++;}

/*
lua_State 中的几个值

StkId top       这个域表示在这个栈上的第一个空闲的slot。
StkId base      这个域表示当前所在函数的base。这个base可以说就是栈底。只不过是当前函数的。
StkId stack_last在栈上的最后一个空闲的slot
StkId stack     栈的base,这个是整个栈的栈底。
StkId是一个Tvalue类型的指针。i
*/

/*
 * 把索引值翻译成实际的地址，返回TValue
 */
static TValue *index2adr (lua_State *L, int idx) {                          如图:
  if (idx > 0) {                                                             ---------
    TValue *o = L->base + (idx - 1);                                        |         | <---- stack_last 最后一个空位置
    api_check(L, idx <= L->ci->top - L->base);                               ---------
    if (o >= L->top) return cast(TValue *, luaO_nilobject);                 |         |
    else return o;                                                           ---------
  }                                                                         |         |
  else if (idx > LUA_REGISTRYINDEX) {                                        ---------
    api_check(L, idx != 0 && -idx <= L->top - L->base);                     |         |
    return L->top + idx;                                                     ---------
  }                                                                         |         |
  else switch (idx) {  /* pseudo-indices */                                  ---------
    case LUA_REGISTRYINDEX: return registry(L);                             |         |
    case LUA_ENVIRONINDEX: {                                                 ---------
      Closure *func = curr_func(L);                                         |         |
      sethvalue(L, &L->env, func->c.env);                                    ---------
      return &L->env;                                                       |         | <---- top   第一个空位置        
    }                                                                        ---------
    case LUA_GLOBALSINDEX: return gt(L);                                    |  used   | <---- index 为 -1 时所取的位置       index2adr 中 idx 参数为正数时，位置为 base + idx - 1
    default: {                                                               ---------
      Closure *func = curr_func(L);                                         |  used   |                                      为负数时，就从上往下找，实际位置为 top + idx
      idx = LUA_GLOBALSINDEX - idx;                                          ---------
      return (idx <= func->c.nupvalues)                                     |  used   |                                      idx 为 0 时表示无效
                ? &func->c.upvalue[idx-1]                                    ---------
                : cast(TValue *, luaO_nilobject);                           |  used   | <---- index 为 2 时所取的位置
    }                                                                        ---------
  }                                                                         |  used   | <---- base  当前函数的位置. 实际就是index 为 1 的地方
}                                                                            ---------
                                                                            |  used   |
                                                                             ---------
                                                                            |  used   |
                                                                             ---------
                                                                            |  used   |
                                                                             ---------
                                                                            |  used   |
                                                                             ---------
                                                                            |  used   |
                                                                             ---------
                                                                            |  used   | <---- stack 整个栈的基地址
                                                                             ---------
static Table *getcurrenv (lua_State *L) {                                   
  if (L->ci == L->base_ci)  /* no enclosing function? */                    
    return hvalue(gt(L));  /* use global table as environment */            
  else {                                                                    
    Closure *func = curr_func(L);                                           
    return func->c.env;                                                     
  }                                                                         
}                                                                           
                                                                            
void luaA_pushobject (lua_State *L, const TValue *o) {                                                                                                                                
  setobj2s(L, L->top, o);
  api_incr_top(L);
}

/*
 * 确保堆栈上至少有 extra 个空位。 如果不能把堆栈扩展到相应的尺寸，函数返回 false 。
 * 这个函数永远不会缩小堆栈； 如果堆栈已经比需要的大了，那么就放在那里不会产生变化
 */
LUA_API int lua_checkstack (lua_State *L, int size) {
  int res = 1;
  lua_lock(L);
  if (size > LUAI_MAXCSTACK || (L->top - L->base + size) > LUAI_MAXCSTACK)
    res = 0;  /* stack overflow */
  else if (size > 0) {
    luaD_checkstack(L, size);
    if (L->ci->top < L->top + size)
      L->ci->top = L->top + size;
  }
  lua_unlock(L);
  return res;
}


LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  int i;
  if (from == to) return;
  lua_lock(to);
  api_checknelems(from, n);
  api_check(from, G(from) == G(to));
  api_check(from, to->ci->top - to->top >= n);
  from->top -= n;
  for (i = 0; i < n; i++) {
    setobj2s(to, to->top++, from->top + i);
  }
  lua_unlock(to);
}


LUA_API void lua_setlevel (lua_State *from, lua_State *to) {
  to->nCcalls = from->nCcalls;
}


/*
 * 不用管这个函数
 */
LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lua_unlock(L);
  return old;
}


/*
 * 创建一个新线程，并将其压入堆栈， 并返回维护这个线程的 lua_State 指针。
 * 这个函数返回的新状态机共享原有状态机中的所有对象（比如一些 table），
 * 但是它有独立的执行堆栈。
 *
 * 没有显式的函数可以用来关闭或销毁掉一个线程。
 * 线程跟其它 Lua 对象一样是垃圾收集的条目之一。
 */
LUA_API lua_State *lua_newthread (lua_State *L) {
  lua_State *L1;
  lua_lock(L);
  luaC_checkGC(L);
  L1 = luaE_newthread(L);
  setthvalue(L, L->top, L1);
  api_incr_top(L);
  lua_unlock(L);
  luai_userstatethread(L, L1);
  return L1;
}



/*
** basic stack manipulation
*/


/*
 * 返回栈顶元素的索引。
 * 因为索引是从 1 开始编号的， 所以这个结果等于堆栈上的元素个数
 * （因此返回 0 表示堆栈为空）
 */
LUA_API int lua_gettop (lua_State *L) {
  return cast_int(L->top - L->base);
}


/*
 * 参数允许传入任何可接受的索引以及 0 。
 * 它将把堆栈的栈顶设为这个索引。
 * 如果新的栈顶比原来的大，超出部分的新元素将被填为 nil 。
 * 如果 index 为 0 ，把栈上所有元素移除
 */
LUA_API void lua_settop (lua_State *L, int idx) {
  lua_lock(L);
  if (idx >= 0) {
    api_check(L, idx <= L->stack_last - L->base);
    while (L->top < L->base + idx)
      setnilvalue(L->top++);
    L->top = L->base + idx;
  }
  else {
    api_check(L, -(idx+1) <= (L->top - L->base));
    L->top += idx+1;  /* `subtract' index (index is negative) */
  }
  lua_unlock(L);
}


/*
 * 从给定有效索引处移除一个元素， 把这个索引之上的所有元素移下来填补上这个空隙。
 * 不能用伪索引来调用这个函数， 因为伪索引并不指向真实的栈上的位置
 */
LUA_API void lua_remove (lua_State *L, int idx) {
  StkId p;
  lua_lock(L);
  p = index2adr(L, idx);
  api_checkvalidindex(L, p);
  while (++p < L->top) setobjs2s(L, p-1, p);
  L->top--;
  lua_unlock(L);
}


/*
 * 把栈顶元素插入指定的有效索引处， 并依次移动这个索引之上的元素。
 * 不要用伪索引来调用这个函数， 因为伪索引不是真正指向堆栈上的位置 
 */
LUA_API void lua_insert (lua_State *L, int idx) {
  StkId p;
  StkId q;
  lua_lock(L);
  p = index2adr(L, idx);
  api_checkvalidindex(L, p);
  for (q = L->top; q>p; q--) setobjs2s(L, q, q-1);
  setobjs2s(L, p, L->top);
  lua_unlock(L);
}


/*
 * 把栈顶元素移动到给定位置（并且把这个栈顶元素弹出）， 
 * 不移动任何元素（因此在那个位置处的值被覆盖掉）
 */
LUA_API void lua_replace (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  /* explicit test for incompatible code */
  if (idx == LUA_ENVIRONINDEX && L->ci == L->base_ci)
    luaG_runerror(L, "no calling environment");
  api_checknelems(L, 1);
  o = index2adr(L, idx);
  api_checkvalidindex(L, o);
  if (idx == LUA_ENVIRONINDEX) {
    Closure *func = curr_func(L);
    api_check(L, ttistable(L->top - 1)); 
    func->c.env = hvalue(L->top - 1);
    luaC_barrier(L, func, L->top - 1);
  }
  else {
    setobj(L, o, L->top - 1);
    if (idx < LUA_GLOBALSINDEX)  /* function upvalue? */
      luaC_barrier(L, curr_func(L), L->top - 1);
  }
  L->top--;
  lua_unlock(L);
}


LUA_API void lua_pushvalue (lua_State *L, int idx) {
  lua_lock(L);
  setobj2s(L, L->top, index2adr(L, idx));
  api_incr_top(L);
  lua_unlock(L);
}



/*
** access functions (stack -> C)
*/


LUA_API int lua_type (lua_State *L, int idx) {
  StkId o = index2adr(L, idx);
  return (o == luaO_nilobject) ? LUA_TNONE : ttype(o);
}


/*
 * 返回 tp 表示的类型名， 这个 tp 必须是 lua_type 可能返回的值中之一
 */
LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  return (t == LUA_TNONE) ? "no value" : luaT_typenames[t];
}


/*
 * 当给定索引的值是一个 C 函数时，返回 1 ，否则返回 0 
 */
LUA_API int lua_iscfunction (lua_State *L, int idx) {
  StkId o = index2adr(L, idx);
  return iscfunction(o);
}


/*
 * 当给定索引的值是一个数字，或是一个可转换为数字的字符串时，返回 1 ，否则返回 0 
 */
LUA_API int lua_isnumber (lua_State *L, int idx) {
  TValue n;
  const TValue *o = index2adr(L, idx);
  return tonumber(o, &n);
}


/*
 * 当给定索引的值是一个字符串或是一个数字（数字总能转换成字符串）时，返回 1 ，否则返回 0 
 */
LUA_API int lua_isstring (lua_State *L, int idx) {
  int t = lua_type(L, idx);
  return (t == LUA_TSTRING || t == LUA_TNUMBER);
}


/*
 * 当给定索引的值是一个 userdata 
 * （无论是完整的 userdata 还是 light userdata ）时，返回 1 ，否则返回 0
 */
LUA_API int lua_isuserdata (lua_State *L, int idx) {
  const TValue *o = index2adr(L, idx);
  return (ttisuserdata(o) || ttislightuserdata(o));
}


/*
 * 如果两个索引 index1 和 index2 处的值简单地相等 （不调用元方法）则返回 1 。
 * 否则返回 0 。
 * 如果任何一个索引无效也返回 0 
 */
LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  StkId o1 = index2adr(L, index1);
  StkId o2 = index2adr(L, index2);
  return (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0
         : luaO_rawequalObj(o1, o2);
}


/*
 * 如果依照 Lua 中 == 操作符语义，
 * 索引 index1 和 index2 中的值相同的话，返回 1 。 否则返回 0 。 
 * 如果任何一个索引无效也会返回 0。 
 */
LUA_API int lua_equal (lua_State *L, int index1, int index2) {
  StkId o1, o2;
  int i;
  lua_lock(L);  /* may call tag method */
  o1 = index2adr(L, index1);
  o2 = index2adr(L, index2);
  i = (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0 : equalobj(L, o1, o2);
  lua_unlock(L);
  return i;
}


/*
 * 如果索引 index1 处的值小于 索引 index2 处的值时，返回 1 ； 否则返回 0 。
 * 其语义遵循 Lua 中的 < 操作符（就是说，有可能调用元方法）。
 * 如果任何一个索引无效，也会返回 0 
 */
LUA_API int lua_lessthan (lua_State *L, int index1, int index2) {
  StkId o1, o2;
  int i;
  lua_lock(L);  /* may call tag method */
  o1 = index2adr(L, index1);
  o2 = index2adr(L, index2);
  i = (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0
       : luaV_lessthan(L, o1, o2);
  lua_unlock(L);
  return i;
}


/*
 * 把制定的数据转成number类型，
 * 其实只有字符串类型可以转，其他类型都不行
 * 十六进制的也OK (带有字母x/X的)
 */

LUA_API lua_Number lua_tonumber (lua_State *L, int idx) {
  TValue n;
  const TValue *o = index2adr(L, idx);

  /**
   * tonumber是看能不能转成数字类型
   * nvalue 就直接把 o 的value设置成n，类型tt 赋值成lua 的 number类型
   * */
  if (tonumber(o, &n))
    return nvalue(o);
  else
    return 0;
}


LUA_API lua_Integer lua_tointeger (lua_State *L, int idx) {
  TValue n;
  const TValue *o = index2adr(L, idx);
  if (tonumber(o, &n)) {
    lua_Integer res;
    lua_Number num = nvalue(o);
    lua_number2integer(res, num);
    return res;
  }
  else
    return 0;
}


LUA_API int lua_toboolean (lua_State *L, int idx) {
  const TValue *o = index2adr(L, idx);
  return !l_isfalse(o);
}


LUA_API const char *lua_tolstring (lua_State *L, int idx, size_t *len) {
  StkId o = index2adr(L, idx);
  if (!ttisstring(o)) {
    lua_lock(L);  /* `luaV_tostring' may create a new string */
    if (!luaV_tostring(L, o)) {  /* conversion failed? */
      if (len != NULL) *len = 0;
      lua_unlock(L);
      return NULL;
    }
    luaC_checkGC(L);
    o = index2adr(L, idx);  /* previous call may reallocate the stack */
    lua_unlock(L);
  }
  if (len != NULL) *len = tsvalue(o)->len;
  return svalue(o);
}


/*
 * 返回指定的索引处的值的长度。
 * 对于 string ，那就是字符串的长度； 
 * 对于 table ，是取长度操作符 ('#') 的结果；
 * 对于 userdata ，就是为其分配的内存块的尺寸； 对于其它值，为 0 。
 */
LUA_API size_t lua_objlen (lua_State *L, int idx) {
  StkId o = index2adr(L, idx);
  switch (ttype(o)) {
    case LUA_TSTRING: return tsvalue(o)->len;
    case LUA_TUSERDATA: return uvalue(o)->len;
    case LUA_TTABLE: return luaH_getn(hvalue(o));
    case LUA_TNUMBER: {
      size_t l;
      lua_lock(L);  /* `luaV_tostring' may create a new string */
      l = (luaV_tostring(L, o) ? tsvalue(o)->len : 0);
      lua_unlock(L);
      return l;
    }
    default: return 0;
  }
}


LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  StkId o = index2adr(L, idx);
  return (!iscfunction(o)) ? NULL : clvalue(o)->c.f;
}


LUA_API void *lua_touserdata (lua_State *L, int idx) {
  StkId o = index2adr(L, idx);
  switch (ttype(o)) {
    case LUA_TUSERDATA: return (rawuvalue(o) + 1);
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  StkId o = index2adr(L, idx);
  return (!ttisthread(o)) ? NULL : thvalue(o);
}


LUA_API const void *lua_topointer (lua_State *L, int idx) {
  StkId o = index2adr(L, idx);
  switch (ttype(o)) {
    case LUA_TTABLE: return hvalue(o);
    case LUA_TFUNCTION: return clvalue(o);
    case LUA_TTHREAD: return thvalue(o);
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
      return lua_touserdata(L, idx);
    default: return NULL;
  }
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  lua_lock(L);
  setnilvalue(L->top);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  lua_lock(L);
  setnvalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


/*
 * 把一个带值的数字放到栈顶
 */
LUA_API void lua_pushinteger (lua_State *L, lua_Integer n) {
  lua_lock(L);
  /*
   * cast_num：把参数 n 强转成 lua_Integer 类型，lua_Integer 实际是 ptrdiff_t 类型，实际就是一个 signed 类型
   * 下面 setnvalue 实际就是下面的操作 
   * L->top->value.n = cast_num(n)
   * L->top->tt = LUA_TNUMBER       LUA_TNUMBER 就是一个数字，标志变量类型的
   */
  setnvalue(L->top, cast_num(n));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlstring (lua_State *L, const char *s, size_t len) {
  lua_lock(L);
  luaC_checkGC(L);
  setsvalue2s(L, L->top, luaS_newlstr(L, s, len));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushstring (lua_State *L, const char *s) {
  if (s == NULL)
    lua_pushnil(L);
  else
    lua_pushlstring(L, s, strlen(s));
}


LUA_API const char *lua_pushvfstring (lua_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lua_lock(L);
  luaC_checkGC(L);
  ret = luaO_pushvfstring(L, fmt, argp);
  lua_unlock(L);
  return ret;
}


LUA_API const char *lua_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lua_lock(L);
  luaC_checkGC(L);
  va_start(argp, fmt);
  ret = luaO_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_unlock(L);
  return ret;
}


LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  Closure *cl;
  lua_lock(L);
  luaC_checkGC(L);
  api_checknelems(L, n);
  cl = luaF_newCclosure(L, n, getcurrenv(L));
  cl->c.f = fn;
  L->top -= n;
  while (n--)
    setobj2n(L, &cl->c.upvalue[n], L->top+n);
  setclvalue(L, L->top, cl);
  lua_assert(iswhite(obj2gco(cl)));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushboolean (lua_State *L, int b) {
  lua_lock(L);
  setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  lua_lock(L);
  setpvalue(L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API int lua_pushthread (lua_State *L) {
  lua_lock(L);
  setthvalue(L, L->top, L);
  api_incr_top(L);
  lua_unlock(L);
  return (G(L)->mainthread == L);
}



/*
** get functions (Lua -> stack)
*/


/*
 * 把 t[k] 值压入堆栈， 这里的 t 是指有效索引 index 指向的值， 而 k 则是栈顶放的值。
 * 这个函数会弹出堆栈上的 key （把结果放在栈上相同位置）。
 * 在 Lua 中，这个函数可能触发对应 "index" 事件的元方法 
 */
LUA_API void lua_gettable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2adr(L, idx);
  api_checkvalidindex(L, t);
  luaV_gettable(L, t, L->top - 1, L->top - 1);
  lua_unlock(L);
}


/*
 * 把 t[k] 值压入堆栈， 这里的 t 是指有效索引 index 指向的值。
 * 在 Lua 中，这个函数可能触发对应 "index" 事件的元方法
 */
LUA_API void lua_getfield (lua_State *L, int idx, const char *k) {
  StkId t;
  TValue key;
  lua_lock(L);
  t = index2adr(L, idx);
  api_checkvalidindex(L, t);
  setsvalue(L, &key, luaS_new(L, k));
  luaV_gettable(L, t, &key, L->top);
  api_incr_top(L);
  lua_unlock(L);
}


/*
 * 类似于 lua_gettable， 但是作一次直接访问（不触发元方法）
 */
LUA_API void lua_rawget (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2adr(L, idx);
  api_check(L, ttistable(t));
  setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1));
  lua_unlock(L);
}


/*
 * 把 t[n] 的值压栈， 这里的 t 是指给定索引 index 处的一个值。
 * 这是一个直接访问；就是说，它不会触发元方法 
 */
LUA_API void lua_rawgeti (lua_State *L, int idx, int n) {
  StkId o;
  lua_lock(L);
  o = index2adr(L, idx);
  api_check(L, ttistable(o));
  setobj2s(L, L->top, luaH_getnum(hvalue(o), n));
  api_incr_top(L);
  lua_unlock(L);
}


/*
 * 创建一个新的空 table 压入堆栈。
 * 这个新 table 将被预分配 narr 个元素的数组空间 以及 nrec 个元素的非数组空间。
 * 当你明确知道表中需要多少个元素时，预分配就非常有用。
 * 如果你不知道，可以使用函数 lua_newtable
 */
LUA_API void lua_createtable (lua_State *L, int narray, int nrec) {
  lua_lock(L);
  luaC_checkGC(L);
  sethvalue(L, L->top, luaH_new(L, narray, nrec));
  api_incr_top(L);
  lua_unlock(L);
}


/*
 * 把给定索引指向的值的元表压入堆栈。
 * 如果索引无效，或是这个值没有元表， 函数将返回 0 并且不会向栈上压任何东西
 */
LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  const TValue *obj;
  Table *mt = NULL;
  int res;
  lua_lock(L);
  obj = index2adr(L, objindex);
  switch (ttype(obj)) {
    case LUA_TTABLE:
      mt = hvalue(obj)->metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(obj)->metatable;
      break;
    default:
      mt = G(L)->mt[ttype(obj)];
      break;
  }
  if (mt == NULL)
    res = 0;
  else {
    sethvalue(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lua_unlock(L);
  return res;
}


/*
 * 把索引处值的环境表压入堆栈。
 */
LUA_API void lua_getfenv (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  o = index2adr(L, idx);
  api_checkvalidindex(L, o);
  switch (ttype(o)) {
    case LUA_TFUNCTION:
      sethvalue(L, L->top, clvalue(o)->c.env);
      break;
    case LUA_TUSERDATA:
      sethvalue(L, L->top, uvalue(o)->env);
      break;
    case LUA_TTHREAD:
      setobj2s(L, L->top,  gt(thvalue(o)));
      break;
    default:
      setnilvalue(L->top);
      break;
  }
  api_incr_top(L);
  lua_unlock(L);
}


/*
** set functions (stack -> Lua)
*/


/*
 * 作一个等价于 t[k] = v 的操作， 这里 t 是一个给定有效索引 index 处的值， v 指栈顶的值， 而 k 是栈顶之下的那个值。
 *
 * 这个函数会把键和值都从堆栈中弹出。 和在 Lua 中一样，这个函数可能触发 "newindex" 事件的元方法 
 */
LUA_API void lua_settable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = index2adr(L, idx);
  api_checkvalidindex(L, t);
  luaV_settable(L, t, L->top - 2, L->top - 1);
  L->top -= 2;  /* pop index and value */
  lua_unlock(L);
}


/*
 * 做一个等价于 t[k] = v 的操作， 这里 t 是给出的有效索引 index 处的值， 而 v 是栈顶的那个值。
 *
 * 这个函数将把这个值弹出堆栈。 跟在 Lua 中一样，这个函数可能触发一个 "newindex" 事件的元方法
 */
LUA_API void lua_setfield (lua_State *L, int idx, const char *k) {
  StkId t;
  TValue key;
  lua_lock(L);
  api_checknelems(L, 1);
  t = index2adr(L, idx);
  api_checkvalidindex(L, t);
  setsvalue(L, &key, luaS_new(L, k));
  luaV_settable(L, t, &key, L->top - 1);
  L->top--;  /* pop value */
  lua_unlock(L);
}


/*
 * 类似于 lua_settable， 但是是作一个直接赋值（不触发元方法）
 */
LUA_API void lua_rawset (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = index2adr(L, idx);
  api_check(L, ttistable(t));
  setobj2t(L, luaH_set(L, hvalue(t), L->top-2), L->top-1);
  luaC_barriert(L, hvalue(t), L->top-1);
  L->top -= 2;
  lua_unlock(L);
}


/*
 * 等价于 t[n] = v， 这里的 t 是指给定索引 index 处的一个值， 而 v 是栈顶的值。
 *
 * 函数将把这个值弹出栈。 赋值操作是直接的；就是说，不会触发元方法。
 */
LUA_API void lua_rawseti (lua_State *L, int idx, int n) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2adr(L, idx);
  api_check(L, ttistable(o));
  setobj2t(L, luaH_setnum(L, hvalue(o), n), L->top-1);
  luaC_barriert(L, hvalue(o), L->top-1);
  L->top--;
  lua_unlock(L);
}


/*
 * 把一个 table 弹出堆栈，并将其设为给定索引处的值的 metatable 
 */
LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  TValue *obj;
  Table *mt;
  lua_lock(L);
  api_checknelems(L, 1);
  obj = index2adr(L, objindex);
  api_checkvalidindex(L, obj);
  if (ttisnil(L->top - 1))
    mt = NULL;
  else {
    api_check(L, ttistable(L->top - 1));
    mt = hvalue(L->top - 1);
  }
  switch (ttype(obj)) {
    case LUA_TTABLE: {
      hvalue(obj)->metatable = mt;
      if (mt)
        luaC_objbarriert(L, hvalue(obj), mt);
      break;
    }
    case LUA_TUSERDATA: {
      uvalue(obj)->metatable = mt;
      if (mt)
        luaC_objbarrier(L, rawuvalue(obj), mt);
      break;
    }
    default: {
      G(L)->mt[ttype(obj)] = mt;
      break;
    }
  }
  L->top--;
  lua_unlock(L);
  return 1;
}


/*
 * 从堆栈上弹出一个 table 并把它设为指定索引处值的新环境。
 * 如果指定索引处的值即不是函数又不是线程或是 userdata ， lua_setfenv 会返回 0 ， 否则返回 1
 */
LUA_API int lua_setfenv (lua_State *L, int idx) {
  StkId o;
  int res = 1;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2adr(L, idx);
  api_checkvalidindex(L, o);
  api_check(L, ttistable(L->top - 1));
  switch (ttype(o)) {
    case LUA_TFUNCTION:
      clvalue(o)->c.env = hvalue(L->top - 1);
      break;
    case LUA_TUSERDATA:
      uvalue(o)->env = hvalue(L->top - 1);
      break;
    case LUA_TTHREAD:
      sethvalue(L, gt(thvalue(o)), hvalue(L->top - 1));
      break;
    default:
      res = 0;
      break;
  }
  if (res) luaC_objbarrier(L, gcvalue(o), hvalue(L->top - 1));
  L->top--;
  lua_unlock(L);
  return res;
}


/*
** `load' and `call' functions (run Lua code)
*/


#define adjustresults(L,nres) \
    { if (nres == LUA_MULTRET && L->top >= L->ci->top) L->ci->top = L->top; }


#define checkresults(L,na,nr) \
     api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)))
	

/*
 * 调用一个函数
 * 调用前，确保函数和参数都已经压入栈中。
 * 函数明最先压栈，接着参数以正序依次进栈
 *
 * 函数调用前，会把函数名和参数出栈；函数调用后，回把返回值正序压栈，故最后一个返回值在栈顶
 * @params    nargs    参数的个数
 * @params    nresults 返回值的个数，这里写几，就把几个返回值压到栈中。如果值为LUA_MULTRET,就有多少个返回值，就压几个
 */
LUA_API void lua_call (lua_State *L, int nargs, int nresults) {
  StkId func;
  lua_lock(L);
  api_checknelems(L, nargs+1);
  checkresults(L, nargs, nresults);
  func = L->top - (nargs+1);
  luaD_call(L, func, nresults);
  adjustresults(L, nresults);
  lua_unlock(L);
}



/*
** Execute a protected call.
*/
struct CallS {  /* data to `f_call' */
  StkId func;
  int nresults;
};


static void f_call (lua_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);
  luaD_call(L, c->func, c->nresults);
}



LUA_API int lua_pcall (lua_State *L, int nargs, int nresults, int errfunc) {
  struct CallS c;
  int status;
  ptrdiff_t func;
  lua_lock(L);
  api_checknelems(L, nargs+1);
  checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else {
    StkId o = index2adr(L, errfunc);
    api_checkvalidindex(L, o);
    func = savestack(L, o);
  }
  c.func = L->top - (nargs+1);  /* function to be called */
  c.nresults = nresults;
  status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
  adjustresults(L, nresults);
  lua_unlock(L);
  return status;
}


/*
** Execute a protected C call.
*/
struct CCallS {  /* data to `f_Ccall' */
  lua_CFunction func;
  void *ud;
};


static void f_Ccall (lua_State *L, void *ud) {
  struct CCallS *c = cast(struct CCallS *, ud);
  Closure *cl;
  cl = luaF_newCclosure(L, 0, getcurrenv(L));
  cl->c.f = c->func;
  setclvalue(L, L->top, cl);  /* push function */
  api_incr_top(L);
  setpvalue(L->top, c->ud);  /* push only argument */
  api_incr_top(L);
  luaD_call(L, L->top - 2, 0);
}


LUA_API int lua_cpcall (lua_State *L, lua_CFunction func, void *ud) {
  struct CCallS c;
  int status;
  lua_lock(L);
  c.func = func;
  c.ud = ud;
  status = luaD_pcall(L, f_Ccall, &c, savestack(L, L->top), 0);
  lua_unlock(L);
  return status;
}


/*
 * 加载一个 Lua chunk 。
 * 如果没有错误， lua_load 把一个编译好的 chunk 作为一个 Lua 函数压入堆栈。
 * 否则，压入出错信息
 */
LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname) {
  ZIO z;
  int status;
  lua_lock(L);
  if (!chunkname) chunkname = "?";
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname);
  lua_unlock(L);
  return status;
}


LUA_API int lua_dump (lua_State *L, lua_Writer writer, void *data) {
  int status;
  TValue *o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = L->top - 1;
  if (isLfunction(o))
    status = luaU_dump(L, clvalue(o)->l.p, writer, data, 0);
  else
    status = 1;
  lua_unlock(L);
  return status;
}


/*
 * 返回线程L的状态
 */
LUA_API int  lua_status (lua_State *L) {
  return L->status;
}


/*
 * 控制垃圾收集器。
 *
 * 这个函数根据其参数 what 发起几种不同的任务：
 *
 * LUA_GCSTOP: 停止垃圾收集器。
 * LUA_GCRESTART: 重启垃圾收集器。
 * LUA_GCCOLLECT: 发起一次完整的垃圾收集循环。
 * LUA_GCCOUNT: 返回 Lua 使用的内存总量（以 K 字节为单位）。
 * LUA_GCCOUNTB: 返回当前内存使用量除以 1024 的余数。
 * LUA_GCSTEP: 发起一步增量垃圾收集。
 *              步数由 data 控制（越大的值意味着越多步）， 而其具体含义（具体数字表示了多少）并未标准化。
 *              如果你想控制这个步数，必须实验性的测试 data 的值。
 *              如果这一步结束了一个垃圾收集周期，返回返回 1 。
 * LUA_GCSETPAUSE: 把 data/100 设置为 garbage-collector pause 的新值（参见 §2.10）。 函数返回以前的值。
 * LUA_GCSETSTEPMUL: 把 arg/100 设置成 step multiplier （参见 §2.10）。 函数返回以前的值。
 *
 */
LUA_API int lua_gc (lua_State *L, int what, int data) {
  int res = 0;
  global_State *g;
  lua_lock(L);
  g = G(L);
  switch (what) {
    case LUA_GCSTOP: {
      g->GCthreshold = MAX_LUMEM;
      break;
    }
    case LUA_GCRESTART: {
      g->GCthreshold = g->totalbytes;
      break;
    }
    case LUA_GCCOLLECT: {
      luaC_fullgc(L);
      break;
    }
    case LUA_GCCOUNT: {
      /* GC values are expressed in Kbytes: #bytes/2^10 */
      res = cast_int(g->totalbytes >> 10);
      break;
    }
    case LUA_GCCOUNTB: {
      res = cast_int(g->totalbytes & 0x3ff);
      break;
    }
    case LUA_GCSTEP: {
      lu_mem a = (cast(lu_mem, data) << 10);
      if (a <= g->totalbytes)
        g->GCthreshold = g->totalbytes - a;
      else
        g->GCthreshold = 0;
      while (g->GCthreshold <= g->totalbytes) {
        luaC_step(L);
        if (g->gcstate == GCSpause) {  /* end of cycle? */
          res = 1;  /* signal it */
          break;
        }
      }
      break;
    }
    case LUA_GCSETPAUSE: {
      res = g->gcpause;
      g->gcpause = data;
      break;
    }
    case LUA_GCSETSTEPMUL: {
      res = g->gcstepmul;
      g->gcstepmul = data;
      break;
    }
    default: res = -1;  /* invalid option */
  }
  lua_unlock(L);
  return res;
}



/*
** miscellaneous functions
*/

/*
 * 产生一个 Lua 错误。
 * 错误信息（实际上可以是任何类型的 Lua 值）必须被置入栈顶。
 * 这个函数会做一次长跳转，因此它不会再返回
 */
LUA_API int lua_error (lua_State *L) {
  lua_lock(L);
  api_checknelems(L, 1);
  luaG_errormsg(L);
  lua_unlock(L);
  return 0;  /* to avoid warnings */
}


LUA_API int lua_next (lua_State *L, int idx) {
  StkId t;
  int more;
  lua_lock(L);
  t = index2adr(L, idx);
  api_check(L, ttistable(t));
  more = luaH_next(L, hvalue(t), L->top - 1);
  if (more) {
    api_incr_top(L);
  }
  else  /* no more elements */
    L->top -= 1;  /* remove key */
  lua_unlock(L);
  return more;
}

/*
 * 连接栈顶的 n 个值， 然后将这些值出栈，并把结果放在栈顶。 
 * 如果 n 为 1 ，结果就是一个字符串放在栈上（即，函数什么都不做）； 
 * 如果 n 为 0 ，结果是一个空串
 * 实际就是字符串链接
 */
LUA_API void lua_concat (lua_State *L, int n) {
  lua_lock(L);
  api_checknelems(L, n);
  if (n >= 2) {
    luaC_checkGC(L);
    luaV_concat(L, n, cast_int(L->top - L->base) - 1);
    L->top -= (n-1);
  }
  else if (n == 0) {  /* push empty string */
    setsvalue2s(L, L->top, luaS_newlstr(L, "", 0));
    api_incr_top(L);
  }
  /* else n == 1; nothing to do */
  lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf (lua_State *L, void **ud) {
  lua_Alloc f;
  lua_lock(L);
  if (ud) *ud = G(L)->ud;
  f = G(L)->frealloc;
  lua_unlock(L);
  return f;
}


LUA_API void lua_setallocf (lua_State *L, lua_Alloc f, void *ud) {
  lua_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lua_unlock(L);
}


/*
 * 这个函数分配分配一块指定大小的内存块，
 * 把内存块地址作为一个完整的 userdata 压入堆栈，并返回这个地址。
 */
LUA_API void *lua_newuserdata (lua_State *L, size_t size) {
  Udata *u;
  lua_lock(L);
  luaC_checkGC(L);
  u = luaS_newudata(L, size, getcurrenv(L));
  setuvalue(L, L->top, u);
  api_incr_top(L);
  lua_unlock(L);
  return u + 1;
}




static const char *aux_upvalue (StkId fi, int n, TValue **val) {
  Closure *f;
  if (!ttisfunction(fi)) return NULL;
  f = clvalue(fi);
  if (f->c.isC) {
    if (!(1 <= n && n <= f->c.nupvalues)) return NULL;
    *val = &f->c.upvalue[n-1];
    return "";
  }
  else {
    Proto *p = f->l.p;
    if (!(1 <= n && n <= p->sizeupvalues)) return NULL;
    *val = f->l.upvals[n-1]->v;
    return getstr(p->upvalues[n-1]);
  }
}


LUA_API const char *lua_getupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val;
  lua_lock(L);
  name = aux_upvalue(index2adr(L, funcindex), n, &val);
  if (name) {
    setobj2s(L, L->top, val);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val;
  StkId fi;
  lua_lock(L);
  fi = index2adr(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(fi, n, &val);
  if (name) {
    L->top--;
    setobj(L, val, L->top);
    luaC_barrier(L, clvalue(fi), L->top);
  }
  lua_unlock(L);
  return name;
}

