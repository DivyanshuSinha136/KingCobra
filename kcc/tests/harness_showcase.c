#include <stdio.h>
#include <stdint.h>
long add(long,long); double many(long,long,long,long,long,long,long,long,double,double);
long fib(long); double hyp(double,double); long sum_to(long); long bump(void); long greet_len(void); long limit(void);
long fill_table(void); long build_and_sum(void); double vec_test(void); long bits(long); long divmod(long,long);
long ufun(uint32_t,uint8_t); long narrow(long); long tern(long); long logic(long,long); double fneg(double); long fcmp(double,double);
long loops(void); long ptrs(void); long say(long); long hidden_get(void); uint64_t u64div(uint64_t,uint64_t); double tofloat(uint64_t);
extern long counter;
int main(){
 printf("add %ld many %.1f fib %ld hyp %.1f sum %ld\n", add(2,40), many(1,2,3,4,5,6,7,8,2.0,0.5), fib(20), hyp(3,4), sum_to(100));
 printf("bump %ld %ld counter %ld greet %ld limit %ld\n", bump(), bump(), counter, greet_len(), limit());
 printf("table %ld list %ld vec %.1f bits %ld divmod %ld\n", fill_table(), build_and_sum(), vec_test(), bits(13), divmod(17,5));
 printf("ufun %ld narrow %ld tern %ld %ld logic %ld %ld %ld\n", ufun(4000000000u, 200), narrow(300), tern(7), tern(3), logic(1,1), logic(5,2), logic(-1,3));
 printf("fneg %.1f fcmp %ld %ld %ld loops %ld ptrs %ld hidden %ld\n", fneg(4.0), fcmp(1,2), fcmp(2,1), fcmp(2,2), loops(), ptrs(), hidden_get());
 say(5);
 printf("u64div %lu tofloat %.1f\n", u64div(18446744073709551615UL, 3), tofloat(18446744073709551615UL));
 return 0; }
