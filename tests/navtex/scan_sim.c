/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 *
 * The decisions of the NAVTEX Scan (src/dialog_navtex_scan.inc) on a
 * recording, on a computer: the scan "arrives" on the recording every STEP
 * seconds after 2 s of silence, waits WAIT seconds, measures the tones for
 * 0.5 s, then waits for the lock and the text as the firmware does.
 *
 *   scan_sim IN.wav [WAIT] [STEP]       (defaults 0.7 s, 15 s)
 *
 * One line per arrival: tone and dip (dB), lock time, good / bad
 * characters, the decision (FOUND, TONE-FAIL, NO-LOCK,
 * TIMEOUT, LOST-LOCK, BAD).  Keep the numbers in step with the .inc.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decoder.h"
#include "sitorb.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* the WAV reader and resampler of navtex_wav.c */
#define main navtex_wav_main
#include "navtex_wav.c"
#undef main
static int good,bad,synced,unsynced; static double lock_t=-1,unsync_t=-1, now;
static void cb(int c,int q,void*u){(void)u; if(q==NAVTEX_Q_EVENT){ if(c==NAVTEX_EV_SYNC_ON){synced=1;unsynced=0; if(lock_t<0) lock_t=now;} else {unsynced=1; unsync_t=now;} return;}
 if(q==NAVTEX_Q_CLEAN||q==NAVTEX_Q_REP) good++; else bad++; }
#define NB 37
int main(int argc,char**argv){
 unsigned rate; long n; float*x=read_wav(argv[1],&rate,&n); if(rate!=12000){long m; float*y=resample(x,n,rate,12000,&m); x=y;n=m;}
 double wait=argc>2?atof(argv[2]):0.7; double step=argc>3?atof(argv[3]):15;
 float coef[NB]; for(int b=0;b<NB;b++) coef[b]=2*cosf(2*M_PI*(550+25*b)/12000);
 srand(1);
 for(double T0=1; T0+1 < n/12000.0; T0+=step){
  sitorb_t*d=sitorb_create(12000,1000,0); sitorb_set_callback(d,cb,NULL);
  float nz[256]; for(int k=0;k<2*12000/256;k++){ for(int i=0;i<256;i++) nz[i]=((rand()/(float)RAND_MAX)-0.5f)*0.01f; sitorb_process(d,nz,256);}
  good=bad=synced=unsynced=0; lock_t=-1;
  double s1[NB]={0},s2[NB]={0},P[NB]={0}; int bn=0,blocks=0; float tone=-99,dip=0,strong=-99; int low=0, measured=0;
  long st=(long)(T0*12000); const char*res="?"; double tt=0; int text=0; double text_t=0; int unseen=0; double us=0;
  for(long i=st;i<n && !(res[0]!='?');i+=256){
    now=(i-st)/12000.0; tt=now;
    // stage: settle
    int nn= (n-i)<256? n-i:256;
    if(!synced && now>=wait && now<wait+0.5){ for(int k=0;k<nn;k++){float v=x[i+k]; for(int b=0;b<NB;b++){double s=v+coef[b]*s1[b]-s2[b]; s2[b]=s1[b]; s1[b]=s;} if(++bn==240){for(int b=0;b<NB;b++){P[b]+=s1[b]*s1[b]+s2[b]*s2[b]-coef[b]*s1[b]*s2[b]; s1[b]=s2[b]=0;} bn=0; blocks++;}} }
    if(now>=wait+0.5 && !measured){ measured=1; double so[NB]; memcpy(so,P,sizeof so); for(int a=0;a<NB;a++)for(int b=a+1;b<NB;b++) if(so[b]<so[a]){double t=so[a];so[a]=so[b];so[b]=t;} double med=so[NB/2]+1e-20;
      for(int f=850;f<=1000;f+=25){int l=(f-550)/25,h=l+6; double hi=P[h]>P[h+1]?P[h]:P[h+1]; double w=P[l]<hi?P[l]:hi; double mid=(P[l+3]+P[l+4])/2+1e-20; float t=10*log10f(w/med); float st=10*log10f((hi>P[l]?hi:P[l])/med); if(t>tone){tone=t;dip=10*log10f(w/mid);low=f;} if(st>strong) strong=st;}
      if(!synced && !(tone>=3 || (strong>=10 && tone>=0))) { res="TONE-FAIL"; break; } }
    sitorb_process(d,x+i,nn);
    if(synced && !text && now>=wait){ text=1; text_t=now; good=bad=0; unseen=0;}
    if(!text && measured && now>wait+0.5+8){res="NO-LOCK";break;}
    if(text){ if(good>=12 && bad*2<=good){res="FOUND";break;}
      int gone=0; if(unsynced){ if(!unseen){unseen=1;us=now;} gone= now-us>=3;} else unseen=0;
      if(gone||now-text_t>=15||(bad>=24&&bad>good)){res=gone?"LOST-LOCK":(now-text_t>=15?"TIMEOUT":"BAD");break;} }
  }
  printf("T0 %6.1f  tone %+5.1f dip %+5.1f (%d Hz)  lock %+5.1f  good %3d bad %3d  t %5.1f  %s\n",T0,tone,dip,low,lock_t,good,bad,tt,res);
  sitorb_destroy(d);
 }
}
