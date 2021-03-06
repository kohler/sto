#include <iostream>
#include <assert.h>
#include <random>
#include <climits>

#include "Array.hh"
#include "Transaction.hh"

// size of array
#define ARRAY_SZ 100
#define NTHREADS 4

// only used for randomRWs test
#define NTRANS 1000000
#define NPERTRANS 10
#define WRITE_PROB .5
#define GLOBAL_SEED 0
#define BLIND_RANDOM_WRITE 0
#define CHECK_RANDOM_WRITES 1

#define MAINTAIN_TRUE_ARRAY_STATE 1

//#define DEBUG

#ifdef DEBUG
#define debug(...) printf(__VA_ARGS__)
#else
#define debug(...) /* */
#endif

typedef Array<int, ARRAY_SZ> ArrayType;
ArrayType *a;

using namespace std;

#if MAINTAIN_TRUE_ARRAY_STATE
bool maintain_true_array_state = true;
int true_array_state[ARRAY_SZ];
#endif

struct Rand {
  typedef uint32_t result_type;

  result_type u, v;
  Rand(result_type u, result_type v) : u(u+1), v(v+1) {}

  inline result_type operator()() {
    v = 36969*(v & 65535) + (v >> 16);
    u = 18000*(u & 65535) + (u >> 16);
    return (v << 16) + u;
  }

  static constexpr result_type max() {
    return (uint32_t)-1;
  }

  static constexpr result_type min() {
    return 0;
  }
};

void *randomRWs(void *p) {
  int me = (intptr_t)p;
  
  std::uniform_int_distribution<> slotdist(0, ARRAY_SZ-1);
  std::uniform_real_distribution<> rwdist(0.,1.);
  uint32_t write_thresh = (uint32_t) (WRITE_PROB * Rand::max());

  for (int i = 0; i < (NTRANS/NTHREADS); ++i) {
    // so that retries of this transaction do the same thing
    auto transseed = i;
#if MAINTAIN_TRUE_ARRAY_STATE
    int slots_written[NPERTRANS], nslots_written;
#endif

    bool done = false;
    while (!done) {
#if MAINTAIN_TRUE_ARRAY_STATE
      nslots_written = 0;
#endif
      Rand transgen(transseed + me + GLOBAL_SEED, transseed + me + GLOBAL_SEED);

      Transaction t;
      for (int j = 0; j < NPERTRANS; ++j) {
        int slot = slotdist(transgen);
        auto r = transgen();
        if (r > write_thresh) {
          a->transRead(t, slot);
        } else {
#if BLIND_RANDOM_WRITE
          a->transWrite(t, slot, j);
#else
          // increment current value (this lets us verify transaction correctness)
          auto v0 = a->transRead(t, slot);
          a->transWrite(t, slot, v0+1);
          ++j; // because we've done a read and a write
#endif
#if MAINTAIN_TRUE_ARRAY_STATE
          slots_written[nslots_written++] = slot;
#endif
        }
      }
      done = t.commit();
      if (!done) {
        debug("thread%d retrying\n", me);
      }
    }
#if MAINTAIN_TRUE_ARRAY_STATE
    if (maintain_true_array_state) {
        std::sort(slots_written, slots_written + nslots_written);
        auto itend = std::unique(slots_written, slots_written + nslots_written);
        for (auto it = slots_written; it != itend; ++it)
            __sync_add_and_fetch(&true_array_state[*it], 1);
    }
#endif
  }
  return NULL;
}

void checkRandomRWs() {
#if !BLIND_RANDOM_WRITE && CHECK_RANDOM_WRITES
  ArrayType *old = a;
  ArrayType check;

  // rerun transactions one-by-one
#if MAINTAIN_TRUE_ARRAY_STATE
  maintain_true_array_state = false;
#endif
  a = &check;
  for (int i = 0; i < NTHREADS; ++i) {
    randomRWs((void*)(intptr_t)i);
  }
#if MAINTAIN_TRUE_ARRAY_STATE
  maintain_true_array_state = true;
#endif
  a = old;

  for (int i = 0; i < ARRAY_SZ; ++i) {
# if MAINTAIN_TRUE_ARRAY_STATE
    if (a->read(i) != true_array_state[i])
        fprintf(stderr, "index [%d]: parallel %d, atomic %d\n",
                i, a->read(i), true_array_state[i]);
# endif
    if (a->read(i) != check.read(i))
        fprintf(stderr, "index [%d]: parallel %d, sequential %d\n",
                i, a->read(i), check.read(i));
    assert(check.read(i) == a->read(i));
  }
#endif
}

void checkIsolatedWrites() {
  for (int i = 0; i < NTHREADS; ++i) {
    assert(a->read(i) == i+1);
  }
}

void *isolatedWrites(void *p) {
  int me = (long long)p;

  bool done = false;
  while (!done) {
    Transaction t;

    for (int i = 0; i < NTHREADS; ++i) {
      a->transRead(t, i);
    }

    a->transWrite(t, me, me+1);
    
    done = t.commit();
    debug("iter: %d %d\n", me, done);
  }
  return NULL;
}


void *blindWrites(void *p) {
  int me = (long long)p;

  bool done = false;
  while (!done) {
    Transaction t;

    if (a->transRead(t, 0) == 0 || me == NTHREADS-1) {
      for (int i = 1; i < ARRAY_SZ; ++i) {
        a->transWrite(t, i, me);
      }
    }

    // NTHREADS-1 always wins
    if (me == NTHREADS-1) {
      a->transWrite(t, 0, me);
    }

    done = t.commit();
    debug("thread %d %d\n", me, done);
  }

  return NULL;
}

void checkBlindWrites() {
  for (int i = 0; i < ARRAY_SZ; ++i) {
    debug("read %d\n", a->read(i));
    assert(a->read(i) == NTHREADS-1);
  }
}

void *interferingRWs(void *p) {
  int me = (long long)p;

  bool done = false;
  while (!done) {
    Transaction t;

    for (int i = 0; i < ARRAY_SZ; ++i) {
      if ((i % NTHREADS) >= me) {
        auto cur = a->transRead(t, i);
        a->transWrite(t, i, cur+1);
      }
    }

    done = t.commit();
    debug("thread %d %d\n", me, done);
  }
  return NULL;
}

void checkInterferingRWs() {
  for (int i = 0; i < ARRAY_SZ; ++i) {
    assert(a->read(i) == (i % NTHREADS)+1);
  }
}

void startAndWait(int n, void *(*start_routine) (void *)) {
  pthread_t tids[n];
  for (int i = 0; i < n; ++i) {
    pthread_create(&tids[i], NULL, start_routine, (void*)(intptr_t)i);
  }

  for (int i = 0; i < n; ++i) {
    pthread_join(tids[i], NULL);
  }
}

struct Test {
  void *(*threadfunc) (void *);
  void (*checkfunc) (void);
};

enum {
  Isolated,
  Blind,
  Interfering,
  Random
};

Test tests[] = {
  {isolatedWrites, checkIsolatedWrites},
  {blindWrites, checkBlindWrites},
  {interferingRWs, checkInterferingRWs},
  {randomRWs, checkRandomRWs}
};



int main(int argc, char *argv[]) {
  if (argc != 2) {
    cout << "Usage: " << argv[0] << " test#" << endl;
    return 1;
  }
  ArrayType stack_arr;
  a = &stack_arr;
  auto test = atoi(argv[1]);
  startAndWait(NTHREADS, tests[test].threadfunc);
  tests[test].checkfunc();
}
