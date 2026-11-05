# std.vector — vector, quaternion and matrix math

```kingcobra
import std.vector;
```

**Representation.** No new value kind: a vector is an ordinary array of
numbers (`[x, y, z]`), a quaternion is `[x, y, z, w]`, and a matrix is a flat
**column-major** array (`mat4` = 16 numbers, `mat3` = 9, `m[col*N + row]`,
the OpenGL/Vulkan convention). Results are always float arrays. Angles are
radians. Everything is plain C with no allocation beyond the result.

## Vectors (any dimension; both operands must have equal length)

| Function | Notes |
|---|---|
| `vec2(x,y)` `vec3(x,y,z)` `vec4(x,y,z,w)` `zero(n)` `fill(n,v)` `copy(v)` | constructors (up to 4096 dims) |
| `add` `sub` `mul` `div` `min` `max` (a,b) | component-wise |
| `neg` `abs` `floor` `ceil` `round` `frac` `sign` (v) | component-wise |
| `scale(v,s)` `clamp(v,lo,hi)` | scalar `lo`/`hi` |
| `dot` `length` `length_sq` `sum` `normalize` | `normalize([0,0,0])` stays zero |
| `distance` `distance_sq` `angle` `lerp(a,b,t)` | `angle` in [0, π] |
| `project(a,onto)` `reject(a,from)` `reflect(v,n)` `refract(v,n,eta)` | `reflect`/`refract` normalise `n`; total internal reflection → zero vector |
| `clamp_length(v,max)` `move_towards(cur,target,max_delta)` `smoothstep(e0,e1,x)` | |
| `cross(a,b)` | 3D → vec3; 2D → scalar z-component |
| `triple(a,b,c)` `perp(v2)` `rotate2(v2,ang)` `rotate3(v3,axis,ang)` | |
| `equals(a,b[,eps])` `is_finite(v)` `deg2rad` `rad2deg` | |
| `barycentric(p,a,b,c)` `ray_sphere(o,d,c,r)` `ray_plane(o,d,p,n)` | ray functions return distance `t ≥ 0` or `null` |

## Quaternions

`quat_identity()`, `quat_from_axis_angle(axis,ang)`, `quat_from_euler(x,y,z)`,
`quat_to_euler(q)`, `quat_mul(a,b)` (applies `b` first), `quat_conjugate`,
`quat_inverse`, `quat_normalize`, `quat_rotate(q,v3)`, `quat_slerp(a,b,t)`
(shortest path), `quat_to_mat4(q)`.

## Matrices

mat4: `mat4_identity`, `mat4_translate(x,y,z)`, `mat4_scale(x,y,z)`,
`mat4_rotate(axis,ang)`, `mat4_rotate_x/y/z(ang)`, `mat4_mul(a,b)` (applies
`b` first), `mat4_transpose`, `mat4_inverse` (error if singular), `mat4_det`,
`mat4_perspective(fovy,aspect,near,far)`, `mat4_ortho(l,r,b,t,n,f)`,
`mat4_look_at(eye,center,up)`, `mat4_mul_vec4(m,v4)`,
`mat4_transform_point(m,p3)` (with perspective divide),
`mat4_transform_dir(m,d3)`, `mat4_trs(translation,quat,scale)`.

mat3: `mat3_identity`, `mat3_mul`, `mat3_det`, `mat3_inverse`,
`mat3_transpose`, `mat3_mul_vec3`, `mat3_from_mat4`, `mat3_normal(model_mat4)`.

Errors (dimension mismatch, wrong component count, singular matrix, degenerate
`look_at`) stop the program with a positioned message, like every other std module.

```kingcobra
let q = vector.quat_from_axis_angle(vector.vec3(0,0,1), vector.deg2rad(90));
print(vector.quat_rotate(q, vector.vec3(1,0,0)));   // ≈ [0, 1, 0]
```

See `examples/std_vector.kc`.
