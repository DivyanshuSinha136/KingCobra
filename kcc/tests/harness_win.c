#include <windows.h>
#include <stdio.h>
typedef long long (*f2)(long long,long long);
typedef long long (*f1)(long long);
typedef double (*fd2)(double,double);
typedef double (*many_t)(long long,long long,long long,long long,long long,long long,long long,long long,double,double);
int main(){
  HMODULE m = LoadLibraryA("show_kcc.dll");
  if(!m){ printf("load failed %lu\n", GetLastError()); return 1; }
  f2 add=(f2)GetProcAddress(m,"add"); f1 fib=(f1)GetProcAddress(m,"fib"); fd2 hyp=(fd2)GetProcAddress(m,"hyp");
  many_t many=(many_t)GetProcAddress(m,"many"); f1 say=(f1)GetProcAddress(m,"say"); f1 gl=(f1)GetProcAddress(m,"greet_len");
  f1 bl=(f1)GetProcAddress(m,"build_and_sum"); f1 pt=(f1)GetProcAddress(m,"ptrs");
  printf("add %lld fib %lld hyp %.1f many %.1f greet %lld list %lld ptrs %lld\n", add(2,40), fib(20), hyp(3,4), many(1,2,3,4,5,6,7,8,2.0,0.5), gl(0), bl(0), pt(0));
  say(7); fflush(stdout);
  long long *c=(long long*)GetProcAddress(m,"counter"); printf("counter %lld\n", *c);
  return 0; }
