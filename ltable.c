/*
** $Id: ltable.c,v 2.32 2006/01/18 11:49:02 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest `n' such that at
** least half the slots between 0 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the `original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
*/

#include <math.h>
#include <string.h>

#define ltable_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"


/*
 * max size of array part is 2^MAXBITS
 * MAXBITS是32位，现在16位机器已经很少了
 * 这里的定义就是为了16位机和32位机的兼容性
 */
#if LUAI_BITSINT > 26
#define MAXBITS		26
#else
#define MAXBITS		(LUAI_BITSINT-2)
#endif

/*
 * 数组部分的最大长度MAXASIZE定义
 */
#define MAXASIZE	(1 << MAXBITS)


/* sizenode(t) 这个在lobject.h文件中定义，取2的(t->lsizenode)次方,
 * 因为hash表的长度肯定是2的倍数，所以lsizenode存放的是长度的log2.
 * sizenode(t) 就是获取hash表的实际长度
 *
 * lmod(s, size) 在lobject.h 中定义,实际是 s % size,只不过是用位运
 * 算实现的. 
 * 这里就是保证 n 不大于sizenode(t)，大于就越界了
 *
 * gnode(t,i) 在ltable.h 中定义, (t)->node[i].就是获取hash表中下表i
 * 的那个Node
 */
#define hashpow2(t,n)      (gnode(t, lmod((n), sizenode(t))))
  
/*
 * 处理字符串用的，以字符串的hash值作为参数，调用上面定义的宏函数
 */
#define hashstr(t,str)  hashpow2(t, (str)->tsv.hash)
/*
 * 同上，给boolean用的，boolean就俩值，1或0，由此可见,对于一个表
 */
#define hashboolean(t,p)        hashpow2(t, p)


/*
 * for some types, it is better to avoid modulus by power of 2, as
 * they tend to have many 2 factors.
 *
 * 实际上和上面的hashpow2(t,n)差不多，具体不同看下例
 *
 * 如果t->lsizenode == 4; 那 lmod 的值不大于2^4(即16),结果范围是0~15
 * 而 hashmode 的值则不大于2^4 - 1(即15), 结果范围是0~14
 *
 * 这是因为很多书2的因子太多，如直接模16，hash表冲突会多，而模15，
 * 冲突就会减少很多。
 */
#define hashmod(t,n)	(gnode(t, ((n) % ((sizenode(t)-1)|1))))

/*
 * 这个是针对指针的, IntPoint(p)在llimits.h中定义, 就是把一个内存地址
 * 强转成unsigned int;
 */
#define hashpointer(t,p)	hashmod(t, IntPoint(p))

/*
 * number of ints inside a lua_Number
 * lua_Number所占内存能包含几个int
 * lua_Number实际是double类型
 *
 * 所以正常32位机，numints应该是2
 */
#define numints		cast_int(sizeof(lua_Number)/sizeof(int))



/* 空Node
 * 内存里一直放一个空的Node, 这个宏就是获取空Node的地址
 */
#define dummynode		(&dummynode_)

/*
 * 空Node的定义
 */
static const Node dummynode_ = {
  {{NULL}, LUA_TNIL},  /* value */
  {{{NULL}, LUA_TNIL, NULL}}  /* key */
};


/* O(1)
 * hash for lua_Numbers
 * 以一个lua_Number,为key,到hash表中取一个Node
 */
static Node *hashnum (const Table *t, lua_Number n) {
  unsigned int a[numints];
  int i;
  n += 1;  /* normalize number (avoid -0) */
  lua_assert(sizeof(a) <= sizeof(n));

  /*下面两行涉及到 lua_Number是如何存储的*/
  memcpy(a, &n, sizeof(a));
  for (i = 1; i < numints; i++) a[0] += a[i];

  return hashmod(t, a[0]);
}

/* O(1)
 * returns the `main' position of an element in a table (that is, the index
 * of its hash value)
 *
 * 根据TValue，获取到hash表中的对应Node链表, lua的hash表使用链表的方式解决冲突的
 * 这个函数只是获取到真正值所在的那个链表
 * 
 * 函数其实就是把所有类型的key,抽象到一块儿了.
 *
 * 只有nil做不了key,其他类型的数据都可以的
 */
static Node *mainposition (const Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TNUMBER:
      /*数字key*/
      return hashnum(t, nvalue(key));
    case LUA_TSTRING:
      /*字符串key*/
      return hashstr(t, rawtsvalue(key));
    case LUA_TBOOLEAN:
      /*boolean类型 作为key*/
      return hashboolean(t, bvalue(key));
    case LUA_TLIGHTUSERDATA:
      /*lightuserdata类型,以它在内存中的地址作为key*/
      return hashpointer(t, pvalue(key));
    default:
      /*其他类型都以其在内存中的地址作为key*/
      return hashpointer(t, gcvalue(key));
  }
}


/* O(1)
 * returns the index for `key' if `key' is an appropriate key to live in
 * the array part of the table, -1 otherwise.
 *
 * 如果参数key是一个数字(这就说明是lua_Number类型,一个double)
 * 把它转成int，然后返回
 *
 * 其他类型，或者类型强转失败了，就返回-1
 */
static int arrayindex (const TValue *key) {

    /*检查是否是一个number类型的TValue*/
  if (ttisnumber(key)) {
    lua_Number n = nvalue(key);
    int k;
    /*lua_number2int 这个宏值得推敲下,在luaconf.h中定义*/
    lua_number2int(k, n);

    /*cast_num再把k转成lua_Number，与原来的n比较，
     * 如果相等，说明转换成功，返回k
     * 如果不相等，说明转换失败了
     */
    if (luai_numeq(cast_num(k), n))
      return k;
  }
  return -1;  /* `key' did not match some condition */
}


/* O(1)
 * returns the index of a `key' for table traversals. First goes all
 * elements in the array part, then elements in the hash part. The
 * beginning of a traversal is signalled by -1.
 *
 * 根据key,返回所要数据在Table中的实际地址,即偏移量
 *
 * 找到了就返回偏移量
 * 有错误发生就返回-1
 */
static int findindex (lua_State *L, Table *t, StkId key) {
  int i;
  /*nil类型不可以作为key,返回-1*/
  if (ttisnil(key)) return -1;  /* first iteration */

  /*lua_Number转成int*/
  i = arrayindex(key);

  /* 这个key是数字，并且再数组下标范围内,
   * 则返回i-1, 因为lua的table是以1开始的不是0开始，
   */
  if (0 < i && i <= t->sizearray)  /* is `key' inside array part? */
    return i-1;  /* yes; that's the index (corrected to C) */
  else {
    /* 不在数组里,那就到hash表中找了
     * 调用mainposition获取到那个链表
     */
    Node *n = mainposition(t, key);

    /*遍历链表对key进行比对*/
    do {  /* check whether `key' is somewhere in the chain */
      /* key may be dead already, but it is ok to use it in `next' */
      /* 英文注释讲得很明白
       *
       * 如果key是个deadkey类型,LUA_TDEADKEY在lobject.h中定义,
       * 对于不是值得数据,有TPROTO,TUPVAL,TDEADKEY三种类型;
       *
       * iscollectable(key)是检测key是否是可回收的,即字符串或GCObject
       * gcvalue是获取数据他们的gc内存地址,
       * 如果链表中的key和参数的key内存地址都一样,那就是找到了
       */
      if (luaO_rawequalObj(key2tval(n), key) ||
            (ttype(gkey(n)) == LUA_TDEADKEY && iscollectable(key) &&
             gcvalue(gkey(n)) == gcvalue(key))) {
        i = cast_int(n - gnode(t, 0));  /* key index in hash table */
        /* hash elements are numbered after array ones 
         * 代码表示hash表在内存中是紧跟着数组部分后面的,即array[arraysize]其实就是Node[0]
         * 但实际是虚拟的,array和node在内存中并不实际的连续
         * 而是对于大于t->sizearray的数,在luaH_next函数(只有它调用findindex)
         * 中, 对于 >= t->sizearray的数,会自动减去t->sizearray,
         * 这就相当于node紧跟在array后面了
         */
        return i + t->sizearray;
      }
      else n = gnext(n);
    } while (n);
    luaG_runerror(L, "invalid key to " LUA_QL("next"));  /* key not found */
    return 0;  /* to avoid warnings */
  }
}


/* O(1)
 * 获取Table中 t[key] 的下一个非nil元素
 * 这个key是栈中的数据
 * 成功返回1，失败返回0
 */
int luaH_next (lua_State *L, Table *t, StkId key) {
  /*根据key获取到元素在table中真正的偏移量*/
  int i = findindex(L, t, key);  /* find original element */

  /* 如果元素在数组部分
   * 第一个i++,是把i设置成下一个元素的地址
   */
  for (i++; i < t->sizearray; i++) {  /* try first array part */
    /*不是nil*/
    if (!ttisnil(&t->array[i])) {  /* a non-nil value? */
      /*把key值设置成next元素的下表i+1(lua数组以0开始)*/
      setnvalue(key, cast_num(i+1));
      /*把next实际元素放到栈中,key的上面*/
      setobj2s(L, key+1, &t->array[i]);
      return 1;
    }
  }
  /*
   * 元素在hash表部分,如果能执行到这里,那么i就已经是t->sizearray了
   * i -= t->sizearray  ===>>> i = 0
   */
  for (i -= t->sizearray; i < sizenode(t); i++) {  /* then hash part */
    if (!ttisnil(gval(gnode(t, i)))) {  /* a non-nil value? */
      /*把链表的首位置的Node的i_key赋值给了key*/
      setobj2s(L, key, key2tval(gnode(t, i)));
      /*把链表的首位置的Node的i_val放到栈中*/
      setobj2s(L, key+1, gval(gnode(t, i)));
      return 1;
    }
  }
  return 0;  /* no more elements */
}


/* O(log(n))
 * {=============================================================
 * Rehash 重置hash表
 * 一般是hash表大小不够了,要重置一下
 * 下面就是实现的一些函数
 * ==============================================================
 */

/* 计算新数组大小, rehash()调用,根据老数组,计算新数组应该有的大小
 * params:
 *  nums[] : 目前Table中各段元素的个数
 *  narray : Table中以数字为key的总元素个数
 *
 * 参数narray被设置成要新数组的大小,
 * 返回值为,要放入数组中的元素个数
 */
static int computesizes (int nums[], int *narray) {
  int i;
  int twotoi;  /* 2^i */
  int a = 0;  /* number of elements smaller than 2^i */
  int na = 0;  /* 要被放到array里数字的个数*/
  int n = 0;  /* 数组大小的最优值, optimal size for array part */
  for (i = 0, twotoi = 1; twotoi/2 < *narray; i++, twotoi *= 2) {
    if (nums[i] > 0) {
      a += nums[i];
      /* 确保数组中使用的个数, 至少占总数组长度的一半 */
      if (a > twotoi/2) {  /* more than half elements present? */
        n = twotoi;  /* 当前最优size, optimal size (till now) */

        /* 原Table中下标小于n的数字都被放到数组中,
         * n肯定是2^i, 这时a值就是twotoi前数字的个数
         */
        na = a;  /* all elements smaller than n will go to array part */
      }
    }
    if (a == *narray) break;  /* all elements already counted */
  }
  *narray = n;
  lua_assert(*narray/2 <= na && na <= *narray);
  return na;
}


/* O(1) 
 * nums[log2(key)]++
 * 前提是key得是个lua_Number,并且不越界
 * 满足了nums数组改变并返回1,否则返回0
 */
static int countint (const TValue *key, int *nums) {
  int k = arrayindex(key);
  if (0 < k && k <= MAXASIZE) {  /* is `key' an appropriate array index? */
    /*ceillog2在lobject.h中定义,为log2(k)*/
    nums[ceillog2(k)]++;  /* count as such */
    return 1;
  }
  else
    return 0;
}


/* O(n) 
 * 计算Table中数组部分非nil元素的个数
 *
 * 参数nums数组中存放各段非nil元素的个数,如果Table中没有nil那么
 * 对于nums数组,rehash()函数里有注释解释
 *
 * 函数返回整个Table数组部分非nil元素个数
 */
static int numusearray (const Table *t, int *nums) {
  int lg;
  int ttlg;  /* 2^lg */
  int ause = 0;  /* summation of `nums' */
  int i = 1;  /* count to traverse all array keys */
  for (lg=0, ttlg=1; lg<=MAXBITS; lg++, ttlg*=2) {  /* for each slice */
    int lc = 0;  /* counter */
    int lim = ttlg;
    if (lim > t->sizearray) {
      lim = t->sizearray;  /* adjust upper limit */
      if (i > lim)
        break;  /* no more elements to count */
    }
    /* count elements in range (2^(lg-1), 2^lg] */
    for (; i <= lim; i++) {
      if (!ttisnil(&t->array[i-1]))
        lc++;
    }
    nums[lg] += lc;
    ause += lc;
  }
  return ause;
}


/* O(n) 
 * 返回hash表中被非nil元素占用的个数
 *
 * 然后根据分段,统计到nums数组中,分段就是2^i ~ 2^(i+1) 为一段,
 * 那么key就属于第log2(key)段
 *
 * 把记录到nums数组中元素的个数加到pnasize上
 * 
 */
static int numusehash (const Table *t, int *nums, int *pnasize) {
  int totaluse = 0;  /* total number of elements 所有非nil元素*/
  int ause = 0;  /* summation of `nums', hash表中以数字作为key的元素个数 */
  /*hash表中最后一个元素下标*/
  int i = sizenode(t);
  while (i--) {
    Node *n = &t->node[i];
    if (!ttisnil(gval(n))) {
      /* 对于key是数字的,nums[log2(n)]++
       * 这样是为了在rehash的时候把这些数字放到array里
       */
      ause += countint(key2tval(n), nums);
      totaluse++;
    }
  }
  *pnasize += ause;
  return totaluse;
}


/* O(1) 
 * 把L中的Table t重新设置成size大小,在原来数组后追加(size - oldsize)块内存
 * 并用循环设置成nil
 */
static void setarrayvector (lua_State *L, Table *t, int size) {
  int i;
  /* 调用luaM_reallocvector 重设数组大小为size 
   * luaM_reallocvector在lmem.h中定义
   * 实际是调用lauxlib.c中的l_alloc()函数,内存分配函数可自己定义的,
   * l_alloc只是系统提供的默认内存分配,对realloc简单的封装
   */
  luaM_reallocvector(L, t->array, t->sizearray, size, TValue);
  /*把新多出来的TValue设置成nil*/
  for (i=t->sizearray; i<size; i++)
     setnilvalue(&t->array[i]);
  t->sizearray = size;
}


/* O(n)
 * 重设hash表, 完全重新申请一块size大小的内存,所有内存初设为nil
 */
static void setnodevector (lua_State *L, Table *t, int size) {
  int lsize;
  /*如果重设大小size位0,那就把Table的node成员指向空链表*/
  if (size == 0) {  /* no elements to hash part? */
    t->node = cast(Node *, dummynode);  /* use common `dummynode' */
    lsize = 0;
  }
  else {
    int i;
    /*lsize = log2(size)*/
    lsize = ceillog2(size);
    if (lsize > MAXBITS)
      luaG_runerror(L, "table overflow");

    /* 重设size,因为本来的size可能是6,34等非2次幂的数
     * 设置size为不小于size的最小2的整数倍
     */
    size = twoto(lsize);

    /*调用lmem.h中定义的luaM_newvector新申请size * sizeof(Node)大小的内存 */
    t->node = luaM_newvector(L, size, Node);

    /*初始化hash表,所有的都是 nil value, 包括Node里的key和value*/
    for (i=0; i<size; i++) {
      Node *n = gnode(t, i);
      gnext(n) = NULL;
      setnilvalue(gkey(n));
      setnilvalue(gval(n));
    }
  }
  t->lsizenode = cast_byte(lsize);
  /*设置lastfree,指向hash表里最后一个空位置*/
  t->lastfree = gnode(t, size);  /* all positions are free */
}

/* O(nhsize)
 * 重置Table大小
 */
static void resize (lua_State *L, Table *t, int nasize, int nhsize) {
  int i;
  int oldasize = t->sizearray;
  int oldhsize = t->lsizenode;
  Node *nold = t->node;  /* save old hash ... */

  /*新size > 旧size, 就用setarrayvector扩充数组*/
  if (nasize > oldasize)  /* array part must grow? */
    setarrayvector(L, t, nasize);

  /* create new hash part with appropriate size */
  /*完全重新申请nhsize大小的内存作为hash表*/
  setnodevector(L, t, nhsize);  
  
  /*如果要把数组大小缩小*/
  if (nasize < oldasize) {  /* array part must shrink? */
    t->sizearray = nasize;
    /* re-insert elements from vanishing slice */
    /* 从nasize开始,后面的元素,也就是数组缩小后,后面被舍弃的元素,
     * 全都放到hash表里 
     * 因为t->sizearray已重新设置,luaH_setnum返回的实际是t->Node 
     * 里的东西
     */
    for (i=nasize; i<oldasize; i++) {
      if (!ttisnil(&t->array[i]))
        setobjt2t(L, luaH_setnum(L, t, i+1), &t->array[i]);
    }
    /* shrink array */
    /*重设到nasize*/
    luaM_reallocvector(L, t->array, oldasize, nasize, TValue);
  }
  /* re-insert elements from hash part */
  for (i = twoto(oldhsize) - 1; i >= 0; i--) {
    Node *old = nold+i;
    if (!ttisnil(gval(old)))
      setobjt2t(L, luaH_set(L, t, key2tval(old)), gval(old));
  }
  /*free掉原hash表*/
  if (nold != dummynode)
    luaM_freearray(L, nold, twoto(oldhsize), Node);  /* free old array */
}

/* O(1)
 * 重设数组大小,
 */ 
void luaH_resizearray (lua_State *L, Table *t, int nasize) {
  int nsize = (t->node == dummynode) ? 0 : sizenode(t);
  resize(L, t, nasize, nsize);
}

/* O(3n + log(n))
 *
 */
static void rehash (lua_State *L, Table *t, const TValue *ek) {
  int nasize, na;/*nasize:以数字作为key的元素的个数*/
  int nums[MAXBITS+1];  /* nums[i] = number of keys between 2^(i-1) and 2^i */
  int i;
  int totaluse;/*Table中所有非nil元素个数*/
  for (i=0; i<=MAXBITS; i++) nums[i] = 0;  /* reset counts */
  /* 先算array */
  nasize = numusearray(t, nums);  /* count keys in array part */
  totaluse = nasize;  /* all those keys are integer keys */
  /* 再算node */
  totaluse += numusehash(t, nums, &nasize);  /* count keys in hash part */
  /* 把参数带的新key也算上 */
  /* count extra key */
  nasize += countint(ek, nums);
  totaluse++;
  /* compute new size for array part */
  na = computesizes(nums, &nasize);
  /* na 就是要放到数组中的元素的个数 
   * nasize 就是新数组的大小 
   * totaluse - na 就是要放入到链表中元素的个数
   */
  /* resize the table to new computed sizes */
  resize(L, t, nasize, totaluse - na);
}



/*
** }=============================================================
*/

/* O(n) 
 * x新创建一个Table
 */
Table *luaH_new (lua_State *L, int narray, int nhash) {
  Table *t = luaM_new(L, Table);
  /* 放到GC里*/
  luaC_link(L, obj2gco(t), LUA_TTABLE);
  t->metatable = NULL;
  t->flags = cast_byte(~0);
  /* temporary values (kept only if some malloc fails) */
  t->array = NULL;
  t->sizearray = 0;
  t->lsizenode = 0;
  t->node = cast(Node *, dummynode);
  setarrayvector(L, t, narray);
  setnodevector(L, t, nhash);
  return t;
}

/* 释放表所占的内存 */
void luaH_free (lua_State *L, Table *t) {
  if (t->node != dummynode)
    luaM_freearray(L, t->node, sizenode(t), Node);
  luaM_freearray(L, t->array, t->sizearray, TValue);
  luaM_free(L, t);
}

/* O(1)
 * 获得hash表中最后一个空位
 */
static Node *getfreepos (Table *t) {
    /*t->lastfree初始值是size,即2^lsizenode*/
  while (t->lastfree-- > t->node) {
    if (ttisnil(gkey(t->lastfree)))
      return t->lastfree;
  }
  return NULL;  /* could not find a free place */
}



/* O(1) 基本是
** inserts a new key into a hash table; first, check whether key's main 
** position is free. If not, check whether colliding node is in its main 
** position or not: if it is not, move colliding node to an empty place and 
** put new key in its main position; otherwise (colliding node is in its main 
 * position), new key goes to an empty position. 
 *
 * hash表中的链表部分,其Node实际也是存放在node[]中的,
 * 就是说,hash表所有元素都放在node[]数组中,而数组元素之间可能存在
 * 着链接关系.
 * 即存在node[a]->next == node[b]node[b]->next == node[c],这中情况
 * 说明这几个元素的hashkey都是a,
 * 而对于有hashkey为b的新元素插入时,就要给node[b]拷贝到一个新位置,
 * 然后把新元素放到node[b]中
 */
static TValue *newkey (lua_State *L, Table *t, const TValue *key) {
  Node *mp = mainposition(t, key);
  if (!ttisnil(gval(mp)) || mp == dummynode) {
    Node *othern;

    /*获得一个空位*/
    Node *n = getfreepos(t);  /* get a free place */

    /*没空位了,hash表已满,rehash*/
    if (n == NULL) {  /* cannot find a free place? */
      rehash(L, t, key);  /* grow table */
      return luaH_set(L, t, key);  /* re-insert key into grown table */
    }
    lua_assert(n != dummynode);
    othern = mainposition(t, key2tval(mp));

    /* 如果node[key]被元素占用了,而这个占用的元素的mainposition并不在这
     * 即它俩hash值不同,不能放在一个链表里
     */
    if (othern != mp) {  /* is colliding node out of its main position? */
      /* yes; move colliding node into free position */
      while (gnext(othern) != mp) othern = gnext(othern);  /* find previous */
      /*把mp复制到新空位n上,但确保原链表不乱*/
      gnext(othern) = n;  /* redo the chain with `n' in place of `mp' */
      *n = *mp;  /* copy colliding node into free pos. (mp->next also goes) */
      gnext(mp) = NULL;  /* now `mp' is free */
      setnilvalue(gval(mp));
    }
    else {  /* colliding node is in its own main position */
      /* new node will go into free position */
      gnext(n) = gnext(mp);  /* chain new position */
      gnext(mp) = n;
      mp = n;
    }
  }
  /*设置新key*/
  gkey(mp)->value = key->value; gkey(mp)->tt = key->tt;
  luaC_barriert(L, t, key);
  lua_assert(ttisnil(gval(mp)));
  return gval(mp);
}


/* O(1)
 * search function for integers
 * key是一个int,先到获取t[key],如果key越界就到hash表中获取
 * 先到array里找,再到hash表中找
 *
 * 这里的key是Lua层的,以为1开始
 */
const TValue *luaH_getnum (Table *t, int key) {
  /* (1 <= key && key <= t->sizearray) */
  if (cast(unsigned int, key-1) < cast(unsigned int, t->sizearray))
    return &t->array[key-1];
  else {
    lua_Number nk = cast_num(key);
    Node *n = hashnum(t, nk);
    do {  /* check whether `key' is somewhere in the chain */
      if (ttisnumber(gkey(n)) && luai_numeq(nvalue(gkey(n)), nk))
        return gval(n);  /* that's it */
      else n = gnext(n);
    } while (n);
    return luaO_nilobject;
  }
}


/* O(1)
 * search function for strings
 * key为string的查找,到hash表里找
 */
const TValue *luaH_getstr (Table *t, TString *key) {
  Node *n = hashstr(t, key);
  do {  /* check whether `key' is somewhere in the chain */
    if (ttisstring(gkey(n)) && rawtsvalue(gkey(n)) == key)
      return gval(n);  /* that's it */
    else n = gnext(n);
  } while (n);
  return luaO_nilobject;
}


/* O(1)
 * main search function
 * 对所有类型的TValue的一个抽象函数,主函数
 * 函数里根据key的不同类型调用不同的函数获取t[key]
 */
const TValue *luaH_get (Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TNIL: return luaO_nilobject;
    case LUA_TSTRING: return luaH_getstr(t, rawtsvalue(key));
    case LUA_TNUMBER: {
      int k;
      lua_Number n = nvalue(key);
      lua_number2int(k, n);
      if (luai_numeq(cast_num(k), nvalue(key))) /* index is int? */
        return luaH_getnum(t, k);  /* use specialized version */
      /* else go through */
    }
    default: {
      Node *n = mainposition(t, key);
      do {  /* check whether `key' is somewhere in the chain */
        if (luaO_rawequalObj(key2tval(n), key))
          return gval(n);  /* that's it */
        else n = gnext(n);
      } while (n);
      return luaO_nilobject;
    }
  }
}

/* O(1)
 * 设置一个新key
 * 1. 如果可以根据key获得一个非nil Value,说明key已存在,那就直接返回这个value
 * 2. 如果获得一个nil TValue,说明这个key在Table中不存在,调用newkey插入一个,
 *    新插入的元素的key由参数指定,值为key的值
 */
TValue *luaH_set (lua_State *L, Table *t, const TValue *key) {
  const TValue *p = luaH_get(t, key);
  t->flags = 0;
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else {
    if (ttisnil(key)) luaG_runerror(L, "table index is nil");
    else if (ttisnumber(key) && luai_numisnan(nvalue(key)))
      luaG_runerror(L, "table index is NaN");
    return newkey(L, t, key);
  }
}

/* O(1)
 * 如果不是一个nil value,就直接返回
 * 如果是nil,就调用newkey生成并返回
 */
TValue *luaH_setnum (lua_State *L, Table *t, int key) {
  const TValue *p = luaH_getnum(t, key);
  if (p != luaO_nilobject)
    return cast(TValue *, p);
        /*好像是从nasize开始,所有元素都往后移动一位*/
  else {
    TValue k;
    setnvalue(&k, cast_num(key));
    return newkey(L, t, &k);
  }
}

/* O(1) 
 * 设置新string key 
 *
 * 如果key存在,返回对应的TValue
 * 否则就new一个新key
 */
TValue *luaH_setstr (lua_State *L, Table *t, TString *key) {
  const TValue *p = luaH_getstr(t, key);
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else {
    TValue k;
    setsvalue(L, &k, key);
    return newkey(L, t, &k);
  }
}

/* O(log(n)) 或 O(nlog(n)) 
 *
 */
static int unbound_search (Table *t, unsigned int j) {
  unsigned int i = j;  /* i is zero or a present index */
  j++;
  /* find `i' and `j' such that i is present and j is not */
  while (!ttisnil(luaH_getnum(t, j))) {
    i = j;
    j *= 2;
    if (j > cast(unsigned int, MAX_INT)) {  /* overflow? */
      /* table was built with bad purposes: resort to linear search */
      i = 1;
      while (!ttisnil(luaH_getnum(t, i))) i++;
      return i - 1;
    }
  }
  /* now do a binary search between them 二分查找
   * 找到第一个为nil的元素
   */ 
  while (j - i > 1) {
    unsigned int m = (i+j)/2;
    if (ttisnil(luaH_getnum(t, m))) j = m;
    else i = m;
  }
  return i;
}


/* O(log(n))
** Try to find a boundary in table `t'. A `boundary' is an integer index
** such that t[i] is non-nil and t[i+1] is nil (and 0 if t[1] is nil).
*/
int luaH_getn (Table *t) {
  unsigned int j = t->sizearray;

  /* 对array从后往前找,找到第一个nil值,就是表的长度了 */
  if (j > 0 && ttisnil(&t->array[j - 1])) {
    /* there is a boundary in the array part: (binary) search for it */
    unsigned int i = 0;
    while (j - i > 1) {
      unsigned int m = (i+j)/2;
      if (ttisnil(&t->array[m - 1])) j = m;
      else i = m;
    }
    return i;
  }
  /* else 说明array是满的或是空的*/
  /* else must find a boundary in hash part */
  else if (t->node == dummynode)  /* hash part is empty? */
    return j;  /* that is easy... */
  else return unbound_search(t, j);
}



#if defined(LUA_DEBUG)

Node *luaH_mainposition (const Table *t, const TValue *key) {
  return mainposition(t, key);
}

int luaH_isdummy (Node *n) { return n == dummynode; }

#endif
