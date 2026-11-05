#include <stdio.h>
#define MS __attribute__((ms_abi))
typedef struct {int a,b;} S1; typedef struct {double x,y;} S2; typedef struct {long a; double d;} S3;
typedef struct {long a,b,c;} S4; typedef struct {unsigned char a;} S5; typedef struct {double d; long a;} S6;
MS S3 c_s3(S3 s){ s.a+=100; s.d+=1.5; return s; }
MS S4 c_s4(S4 s,long k){ s.a+=k; s.b+=k; s.c+=k; return s; }
MS S2 c_s2(S2 a,S2 b){ S2 r={a.x*b.x,a.y-b.y}; return r; }
MS S1 k_s1(S1,long); MS S2 k_s2(S2,S2); MS S3 k_s3(S3,long); MS S4 k_s4(S4,long); MS S5 k_s5(S5); MS S6 k_s6(S6);
MS long k_many(S4,S4,S3,S2,S1,long,long,long,long,long,long,long);
MS double k_via_c(S3); MS long k_via_c4(S4); MS double k_via_c2(S2,S2);
int main(){
  S1 a={3,4}; S1 r1=k_s1(a,10); printf("%d %d\n",r1.a,r1.b);
  S2 x={1.5,2.0},y={0.5,3.0}; S2 r2=k_s2(x,y); printf("%g %g\n",r2.x,r2.y);
  S3 s3={7,2.5}; S3 r3=k_s3(s3,3); printf("%ld %g\n",r3.a,r3.d);
  S4 s4={1,2,3}; S4 r4=k_s4(s4,5); printf("%ld %ld %ld\n",r4.a,r4.b,r4.c);
  S5 s5={200}; S5 r5=k_s5(s5); printf("%d\n",r5.a);
  S6 s6={1.25,9}; S6 r6=k_s6(s6); printf("%g %ld\n",r6.d,r6.a);
  S4 b4={10,20,30}; S1 e={5,6};
  printf("%ld\n",k_many(s4,b4,s3,x,e,1,2,3,4,5,6,7));
  printf("%g %ld %g\n",k_via_c(s3),k_via_c4(s4),k_via_c2(x,y));
  return 0; }
