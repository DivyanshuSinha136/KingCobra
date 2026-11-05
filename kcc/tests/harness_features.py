import ctypes as c
l=c.CDLL("./feat.so")
def f(name, res=c.c_long, *args):
    fn=getattr(l,name); fn.restype=res; fn.argtypes=list(args) or None; return fn
L=c.c_long; D=c.c_double
print("even/odd", f("is_even",c.c_bool,L)(10), f("is_odd",c.c_bool,L)(7))
print("out", f("use_out")())
print("stack", f("stack_args",L,*[L]*9)(*range(1,10)), "expect", sum((i+1)*(i+1) for i in range(9)))
print("fstack", f("float_stack",D,*[D]*9,L)(1,2,3,4,5,6,7,8,9.0,2), "expect", 36+18)
print("grid", f("grid_sum")(), "expect", 10+11+12+20+21+22+7)
print("nested", f("nested_struct")(), "expect", -100)
print("f32", f("f32_store",D)(), "expect 3.875")
print("compound", f("compound")())
print("chars", f("chars",L,L)(1), "expect", ord('b')+ord('a')+10)
print("pp", f("pp")(), "bool", f("bool_fn",L,c.c_bool,c.c_bool)(True,False), f("bool_fn",L,c.c_bool,c.c_bool)(True,True))
print("negmod", f("neg_mod",L,L,L)(-7,3), "shifts", f("shifts",c.c_uint64,c.c_uint64)(0xF000000000000000), f("sshift",L,L)(-8))
print("gpair", f("pair_global")(), "void", f("call_void")(), "u8cmp", f("u8_cmp",L,c.c_uint8,c.c_uint8)(3,200), f("u8_cmp",L,c.c_uint8,c.c_uint8)(200,3))
print("ptr", f("ptr_arith")(), "expect", 33+3000+22, "sizes", f("sizes")(), "big", f("big_frame")())
