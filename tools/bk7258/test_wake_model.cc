/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <vision_badge/wake_model.h>
#include <vision_badge/wake_policy.h>
#ifdef CMSIS_NN
#include <arm_nnsupportfunctions.h>
#endif
extern "C" {
extern const unsigned char r1_wake_model_data[];
extern const size_t r1_wake_model_bytes;
static int allocations, fail_allocation;
void *bk7258_psram_malloc(size_t bytes) { if (fail_allocation) return nullptr; allocations++; return std::malloc(bytes); }
void bk7258_psram_free(void *p) { if (p) {allocations--; std::free(p);} }
}
int main(int argc,char **argv)
{
#ifdef CMSIS_NN
  const int32_t edge_values[]={INT32_MIN,INT32_MIN+1,-1073741824,-1,0,1,1073741823,INT32_MAX};
  for(int shift=0;shift<=31;shift++) for(int32_t value:edge_values)
    {
      int64_t exact=static_cast<int64_t>(value)*(INT64_C(1)<<shift);
      int32_t expected=exact>INT32_MAX?INT32_MAX:exact<INT32_MIN?INT32_MIN:static_cast<int32_t>(exact);
      assert(arm_nn_mult_by_power_of_two(value,shift)==expected);
    }
  puts("CMSIS_SATURATING_SHIFT_PASS signed endpoints, negative values, shifts 0..31");
#endif
  r1_wake_policy_s policy;
  float hot[3]={.05f,.05f,.9f}, quiet[3]={.8f,.1f,.1f}, invalid_scores[3]={NAN,0,1};
  r1_wake_policy_init(&policy,false);
  assert(r1_wake_policy_score(&policy,hot,1000)==0);
  assert(r1_wake_policy_score(&policy,hot,1300)==1);
  assert(r1_wake_policy_score(&policy,hot,1600)==0);
  assert(r1_wake_policy_score(&policy,quiet,1900)==0 && !policy.armed);
  assert(r1_wake_policy_score(&policy,quiet,2300)==0 && policy.armed);
  assert(r1_wake_policy_score(&policy,hot,2600)==0);
  r1_wake_policy_gap(&policy);
  assert(r1_wake_policy_score(&policy,hot,2900)==0);
  assert(r1_wake_policy_score(&policy,hot,3200)==1);
  assert(r1_wake_policy_score(&policy,invalid_scores,3500)==-EPROTO);
  assert(r1_wake_policy_score(&policy,hot,3100)==-ESTALE);
  r1_wake_policy_init(&policy,true);
  assert(r1_wake_policy_score(&policy,hot,10000)==0);
  assert(r1_wake_policy_score(&policy,quiet,10300)==0);
  assert(r1_wake_policy_score(&policy,hot,10600)==0);
  assert(r1_wake_policy_score(&policy,hot,10900)==1);
  r1_wake_policy_init(&policy,false);
  assert(r1_wake_policy_score(&policy,hot,12000)==0);
  assert(r1_wake_policy_score(&policy,hot,12600)==1);
  r1_wake_policy_init(&policy,false);
  assert(r1_wake_policy_score(&policy,hot,14000)==0);
  assert(r1_wake_policy_score(&policy,hot,14701)==0); /* Missed evaluation clears hits. */
  assert(r1_wake_policy_score(&policy,hot,15301)==1);
  float medium[3]={.15f,.15f,.70f};
  r1_wake_policy_init(&policy,false);
  assert(policy.threshold_percent==85 && r1_wake_policy_set_threshold(&policy,49)==-EINVAL);
  assert(r1_wake_policy_score(&policy,medium,16000)==0 && !policy.hits);
  assert(!r1_wake_policy_set_threshold(&policy,60));
  assert(r1_wake_policy_score(&policy,medium,16600)==0);
  assert(r1_wake_policy_score(&policy,medium,17200)==1);
  assert(!r1_wake_policy_set_threshold(&policy,85) && !policy.armed);
  assert(r1_wake_policy_score(&policy,quiet,18400)==0 && policy.armed);
  puts("WAKE_POLICY_PASS consecutive hits, gap resets, cooldown/release, finite scores and monotonic clock");
  r1_wake_model_s *m=nullptr;
  int selftest_output[3];
  assert(r1_wake_model_selftest(nullptr)==-EINVAL);
  assert(r1_wake_model_selftest(selftest_output)==0 && !allocations);
  unsigned int changed, max_delta;
  assert(r1_wake_model_frontend_selftest(nullptr,&max_delta)==-EINVAL);
  assert(!r1_wake_model_frontend_selftest(&changed,&max_delta) && !changed && !max_delta && !allocations);
  puts("WAKE_FRONTEND_SELFTEST_PASS fixed PCM produces exact 40-bin host fixture");
  puts("WAKE_FIXED_INPUT_SELFTEST_PASS backend output matches reviewed golden tensor");
  assert(r1_wake_model_open(nullptr,0,false,&m)==-EINVAL);
  unsigned char invalid[32]={};
  assert(r1_wake_model_open(invalid,sizeof(invalid),false,&m)==-EBADMSG && !allocations);
  fail_allocation=1;
  assert(r1_wake_model_frontend_selftest(&changed,&max_delta)==-ENOMEM && !allocations);
  assert(r1_wake_model_selftest(selftest_output)==-ENOMEM && !allocations);
  assert(r1_wake_model_open(r1_wake_model_data,r1_wake_model_bytes,false,&m)==-ENOMEM && !allocations);
  fail_allocation=0;
  assert(!std::memcmp(r1_wake_model_data,"R1PB",4));
  auto *damaged=static_cast<unsigned char *>(std::malloc(r1_wake_model_bytes));
  assert(damaged);
  const size_t damaged_offsets[]={4,8,r1_wake_model_bytes-1};
  for(size_t offset: damaged_offsets)
    {
      std::memcpy(damaged,r1_wake_model_data,r1_wake_model_bytes);damaged[offset]^=1;
      assert(r1_wake_model_open(damaged,r1_wake_model_bytes,false,&m)==-EBADMSG && !m && !allocations);
    }
  assert(r1_wake_model_open(r1_wake_model_data,r1_wake_model_bytes-1,false,&m)==-EBADMSG && !m && !allocations);
  std::free(damaged);
  puts("WAKE_PACKED_MODEL_REJECTION_PASS length, checksum, payload corruption and truncation");
  auto *caller_model=static_cast<unsigned char *>(std::malloc(r1_wake_model_bytes));
  assert(caller_model);
  std::memcpy(caller_model,r1_wake_model_data,r1_wake_model_bytes);
  assert(r1_wake_model_open(caller_model,r1_wake_model_bytes,false,&m)==0 && m);
  /* Loading owns its aligned copy, as in the reference model loader. Destroy
   * the source before any Invoke so ASan catches an accidental retained
   * FlatBuffer/weight pointer into the caller's storage. */
  std::memset(caller_model,0,r1_wake_model_bytes);
  std::free(caller_model);
  assert(r1_wake_model_arena_used(m)>0 && r1_wake_model_arena_used(m)<=64*1024);
  r1_wake_model_diag_s diagnostic;
  assert(r1_wake_model_diagnostics(m,&diagnostic)==-ENODATA);
  int16_t pcm[320]={}; float score; bool inferred; unsigned windows=0;
  for(unsigned i=1;i<=400;i++)
    {
      assert(r1_wake_model_feed(m,pcm,i*20,&score,&inferred)==0);
      if (i<R1_WAKE_SCORE_PERIOD_HOPS) assert(!inferred);
      if (i==R1_WAKE_SCORE_PERIOD_HOPS) assert(inferred);
      if(inferred) { windows++; assert(std::isfinite(score) && score<.85f); }
    }
  assert(windows==400/R1_WAKE_SCORE_PERIOD_HOPS);
  assert(!r1_wake_model_diagnostics(m,&diagnostic) && diagnostic.valid);
  assert(diagnostic.input_low+diagnostic.input_high<=5960 && diagnostic.scores_milli[2]<850);
  assert(r1_wake_model_feed(m,pcm,8000,&score,&inferred)==-ESTALE);
  r1_wake_model_gap(m);
  assert(r1_wake_model_diagnostics(m,&diagnostic)==-ENODATA);
  for(unsigned i=1;i<=150;i++)
    {
      for(unsigned s=0;s<320;s++) pcm[s]=static_cast<int16_t>(2000*std::sin((i*320+s)*.07));
      int ret=r1_wake_model_feed(m,pcm,9000+i*20,&score,&inferred);
      assert(ret>=0); /* A tone is an input/quantization test, not a speech benchmark. */
      if(i<150) assert(!inferred);
    }
  printf("WAKE_REAL_MODEL_PASS silence_windows=%u arena_used=%zu bytes model=%zu; frontend and inference real, synthetic PCM, no human accuracy claim\n",windows,r1_wake_model_arena_used(m),r1_wake_model_bytes);
  r1_wake_model_close(m);assert(!allocations);
  if(argc>=2)
    {
      FILE *file=std::fopen(argv[1],"rb");assert(file);
      FILE *scores=argc==3?std::fopen(argv[2],"w"):nullptr;assert(argc!=3 || scores);
      assert(r1_wake_model_open(r1_wake_model_data,r1_wake_model_bytes,false,&m)==0);
      unsigned frames=0, triggers=0, evaluations=0;float maximum=0;
      std::memset(pcm,0,sizeof(pcm));
      while(std::fread(pcm,sizeof(int16_t),320,file)==320)
        {
          int ret=r1_wake_model_feed(m,pcm,++frames*20,&score,&inferred);assert(ret>=0);
          if(ret==1)triggers++;
          if(inferred){evaluations++;if(score>maximum)maximum=score;
            if(scores)std::fprintf(scores,"%u,%d,%d\n",frames,static_cast<int>(std::round(score*256)),ret==1);}
        }
      assert(!std::ferror(file));std::fclose(file);
      if(scores){assert(!std::ferror(scores));std::fclose(scores);}
      printf("WAKE_SYNTHETIC_SMOKE frames=%u evaluations=%u triggers=%u max_score=%.6f; synthesized voice only, not human/board acceptance\n",frames,evaluations,triggers,maximum);
      r1_wake_model_close(m);assert(!allocations);
    }
}
