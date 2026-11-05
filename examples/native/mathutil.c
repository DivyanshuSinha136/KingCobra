/* A tiny native library used by examples/native_import_demo.kc.
 * Must be compiled with -fPIC so it can later be linked into a
 * shared library (KingCobra does this automatically for a .o import).
 *   cc -fPIC -c mathutil.c -o mathutil.o
 */
long square(long x) { return x * x; }
long cube(long x) { return x * x * x; }
double half(double x) { return x / 2.0; }
