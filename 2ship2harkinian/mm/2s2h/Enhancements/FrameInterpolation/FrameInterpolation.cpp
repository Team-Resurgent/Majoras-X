#include <libultraship/bridge/consolevariablebridge.h>

#include <vector>
#include <map>
#include <unordered_map>
#include <math.h>

#include "FrameInterpolation.h"
#include "2s2h/BenPort.h"
#include <sys_matrix.h>
#include <z64skin_matrix.h>

/*
Frame interpolation.

The idea of this code is to interpolate all matrices.

The code contains two approaches. The first is to interpolate
all inputs in transformations, such as angles, scale and distances,
and then perform the same transformations with the interpolated values.
After evaluation for some reason some animations such rolling look strange.

The second approach is to simply interpolate the final matrices. This will
more or less simply interpolate the world coordinates for movements.
This will however make rotations ~180 degrees get the "paper effect".
The mitigation is to identify this case for actors and interpolate the
matrix but in model coordinates instead, by "removing" the rotation-
translation before interpolating, create a rotation matrix with the
interpolated angle which is then applied to the matrix.

Currently the code contains both methods but only the second one is currently
used.

Both approaches build a tree of instructions, containing matrices
at leaves. Every node is built from OPEN_DISPS/CLOSE_DISPS and manually
inserted FrameInterpolation_OpenChild/FrameInterpolation_Close child calls.
These nodes contain information that should suffice to identify the matrix,
so we can find it in an adjacent frame.

We can interpolate an arbitrary amount of frames between two original frames,
given a specific interpolation factor (0=old frame, 0.5=average of frames,
1.0=new frame).
*/

static bool invert_matrix(const float m[16], float invOut[16]);

using namespace std;

namespace {

enum class Op {
    OpenChild,
    CloseChild,

    MatrixPush,
    MatrixPop,
    MatrixPut,
    MatrixMult,
    MatrixTranslate,
    MatrixScale,
    MatrixRotate1Coord,
    MatrixRotateZYX,
    MatrixTranslateRotateZYX,
    MatrixSetTranslateRotateYXZ,
    MatrixMtxFToMtx,
    MatrixToMtx,
    MatrixReplaceRotation,
    MatrixRotateAxis,
    SkinMatrixMtxFToMtx
};

typedef pair<const void*, int> label;

// A recording's nodes are made one at a time and go all at once, a tick later: thousands of heap calls a tick, and
// a walk of the old tree to free it, where its arena just starts again. Two arenas, as a recording outlives the
// next one's start; their chunks are kept, so the steady cost is the peak's memory and no calls at all.
class Arena {
  public:
    void* Get(size_t bytes) {
        bytes = (bytes + 7) & ~(size_t)7;
        if (bytes > kChunk / 4) { // (a long vector's: on its own)
            mBig.push_back(::operator new(bytes));
            mBigBytes += bytes;
            return mBig.back();
        }
        if (mUsed + bytes > kChunk) {
            mChunk++;
            mUsed = 0;
        }
        if (mChunk == mChunks.size()) {
            mChunks.push_back(::operator new(kChunk));
        }
        void* p = (char*)mChunks[mChunk] + mUsed;
        mUsed += bytes;
        return p;
    }

    void Reset() {
        for (void* p : mBig) {
            ::operator delete(p);
        }
        mBig.clear();
        mBigBytes = 0;
        mChunk = mUsed = 0;
    }

    size_t Bytes() const {
        return mChunks.size() * kChunk + mBigBytes;
    }

  private:
    static constexpr size_t kChunk = 64 * 1024;
    vector<void*> mChunks, mBig;
    size_t mChunk = 0, mUsed = 0, mBigBytes = 0;
};

Arena arenas[2];
int arena_now;

template <class T> struct ArenaAlloc {
    using value_type = T;
    ArenaAlloc() = default;
    template <class U> ArenaAlloc(const ArenaAlloc<U>&) {
    }
    T* allocate(size_t n) {
        return (T*)arenas[arena_now].Get(n * sizeof(T));
    }
    void deallocate(T*, size_t) {
    }
    template <class U> bool operator==(const ArenaAlloc<U>&) const {
        return true;
    }
    template <class U> bool operator!=(const ArenaAlloc<U>&) const {
        return false;
    }
};

template <class T> using ArenaVector = vector<T, ArenaAlloc<T>>;
template <class K, class V> using ArenaMap = map<K, V, less<K>, ArenaAlloc<pair<const K, V>>>;

union Data {
    Data() {
    }

    struct {
        MtxF src;
    } matrix_put;

    struct {
        MtxF mf;
        u8 mode;
    } matrix_mult;

    struct {
        f32 x, y, z;
        u8 mode;
    } matrix_translate, matrix_scale;

    struct {
        u32 coord;
        f32 value;
        u8 mode;
    } matrix_rotate_1_coord;

    struct {
        s16 x, y, z;
        u8 mode;
    } matrix_rotate_zyx;

    struct {
        Vec3f translation;
        Vec3s rotation;
    } matrix_translate_rotate_zyx;

    struct {
        f32 translateX, translateY, translateZ;
        Vec3s rot;
        // MtxF mtx;
        bool has_mtx;
        bool interpolate_wider_angles;
    } matrix_set_translate_rotate_yxz;

    struct {
        MtxF src;
        Mtx* dest;
    } matrix_mtxf_to_mtx;

    struct {
        Mtx* dest;
        MtxF src;
        // The matrix as it actually was for this frame, before any actor-relative adjustment. Kept around
        // (regardless of has_adjusted) so a has_adjusted mismatch between frames has a correct, non-relative
        // matrix to fall back to instead of blending across incompatible spaces.
        MtxF raw;
        bool has_adjusted;
    } matrix_to_mtx;

    struct {
        MtxF mf;
    } matrix_replace_rotation;

    struct {
        f32 angle;
        Vec3f axis;
        u8 mode;
    } matrix_rotate_axis;

    struct {
        label key;
        size_t idx;
    } open_child;
};

struct Path {
    ArenaMap<label, ArenaVector<Path>> children;
    ArenaMap<Op, ArenaVector<Data>> ops;
    ArenaVector<pair<Op, size_t>> items;
};

// The replacements are made from the matrices recorded at MatrixToMtx and MatrixMtxFToMtx; of the matrix stack only
// actor_mtx is read, and it's set whole by SetTranslateRotateYXZ. So the stack's other operations needn't be recorded
// or run again: on the Xbox they were two full walks of every limb's sines and products a tick, for nothing.
#ifdef LUS_XBOX
constexpr bool kRecordMatrixStack = false;
#else
constexpr bool kRecordMatrixStack = true;
#endif

bool is_recording;
vector<Path*> current_path;
uint32_t camera_epoch;
uint32_t previous_camera_epoch;
Path no_recording; // (before the first; a recording's root is in its arena, and never destroyed: Arena)
Path* current_recording = &no_recording;
Path* previous_recording = &no_recording;

bool interpolate_wider_angles = false;

bool next_is_actor_pos_rot_matrix;
bool has_inv_actor_mtx;
bool ignore_inv_actor_mtx;
size_t ignore_inv_actor_mtx_path_index;
MtxF inv_actor_mtx;
size_t inv_actor_mtx_path_index;

// the arena of the recording before the last: nothing reads it now
Path* new_recording() {
    arena_now ^= 1;
    arenas[arena_now].Reset();
    return new (arenas[arena_now].Get(sizeof(Path))) Path();
}

Data& append(Op op) {
    auto& m = current_path.back()->ops[op];
    current_path.back()->items.emplace_back(op, m.size());
    return m.emplace_back();
}

struct InterpolateCtx {
    float step;
    float w;
#ifdef LUS_XBOX
    MtxBlends* mtx_replacements;
#else
    unordered_map<Mtx*, MtxF>* mtx_replacements;
#endif
    MtxF tmp_mtxf, tmp_mtxf2;
    Vec3f tmp_vec3f;
    Vec3s tmp_vec3s;
    MtxF actor_mtx;

    MtxF* new_replacement(Mtx* addr) {
#ifdef LUS_XBOX
        return mtx_replacements->Put(addr);
#else
        return &(*mtx_replacements)[addr];
#endif
    }

    void interpolate_mtxf(MtxF* res, MtxF* o, MtxF* n) {
        for (size_t i = 0; i < 4; i++) {
            for (size_t j = 0; j < 4; j++) {
                res->mf[i][j] = w * o->mf[i][j] + step * n->mf[i][j];
            }
        }
    }

    float lerp(f32 o, f32 n) {
        return w * o + step * n;
    }

    void lerp_vec3f(Vec3f* res, Vec3f* o, Vec3f* n) {
        res->x = lerp(o->x, n->x);
        res->y = lerp(o->y, n->y);
        res->z = lerp(o->z, n->z);
    }

    float interpolate_angle(f32 o, f32 n) {
        if (o == n)
            return n;
        o = fmodf(o, 2 * M_PI);
        if (o < 0.0f) {
            o += 2 * M_PI;
        }
        n = fmodf(n, 2 * M_PI);
        if (n < 0.0f) {
            n += 2 * M_PI;
        }
        if (fabsf(o - n) > M_PI) {
            if (o < n) {
                o += 2 * M_PI;
            } else {
                n += 2 * M_PI;
            }
        }
        if (fabsf(o - n) > M_PI / 2) {
            // return n;
        }
        return lerp(o, n);
    }

    s16 interpolate_angle(s16 os, s16 ns) {
        if (os == ns)
            return ns;
        int o = (u16)os;
        int n = (u16)ns;
        u16 res;
        int diff = o - n;
        if (-0x8000 <= diff && diff <= 0x8000) {
            if (diff < -0x4000 || diff > 0x4000) {
                // Wider angle cut off values are just slightly larger than when Deku Link enters a flower
                if (!interpolate_wider_angles || diff < -0x5700 || diff > 0x5700) {
                    return ns;
                }
            }
            res = (u16)(w * o + step * n);
        } else {
            if (o < n) {
                o += 0x10000;
            } else {
                n += 0x10000;
            }
            diff = o - n;
            if (diff < -0x4000 || diff > 0x4000) {
                if (!interpolate_wider_angles || diff < -0x5700 || diff > 0x5700) {
                    return ns;
                }
            }
            res = (u16)(w * o + step * n);
        }
        if (os / 327 == ns / 327 && (s16)res / 327 != os / 327) {
            int bp = 0;
        }
        return res;
    }

    void interpolate_angles(Vec3s* res, Vec3s* o, Vec3s* n) {
        res->x = interpolate_angle(o->x, n->x);
        res->y = interpolate_angle(o->y, n->y);
        res->z = interpolate_angle(o->z, n->z);
    }

    void interpolate_branch(Path* old_path, Path* new_path) {
        const bool self_paired = old_path == new_path;
        // (runs of one kind of operation are the rule: the maps looked in once for each run)
        Op run_op = Op::CloseChild;
        ArenaVector<Data>* new_ops = nullptr;
        ArenaVector<Data>* old_ops = nullptr;

        for (auto& item : new_path->items) {
            if (new_ops == nullptr || item.first != run_op) {
                auto old_it = old_path->ops.find(item.first);
                run_op = item.first;
                new_ops = &new_path->ops[item.first];
                old_ops = old_it != old_path->ops.end() ? &old_it->second : nullptr;
            }
            Data& new_op = (*new_ops)[item.second];

            if (item.first == Op::OpenChild) {
                Path* new_child = &new_path->children.find(new_op.open_child.key)->second[new_op.open_child.idx];
                if (auto it = old_path->children.find(new_op.open_child.key);
                    it != old_path->children.end() && new_op.open_child.idx < it->second.size()) {
                    interpolate_branch(&it->second[new_op.open_child.idx], new_child);
                } else {
                    interpolate_branch(new_child, new_child);
                }
                continue;
            }

            if (old_ops != nullptr) {
                if (item.second < old_ops->size()) {
                    Data& old_op = (*old_ops)[item.second];
                    switch (item.first) {
                        case Op::OpenChild:
                            break;
                        case Op::CloseChild:
                            break;

                        case Op::MatrixPush:
                            Matrix_Push();
                            break;

                        case Op::MatrixPop:
                            Matrix_Pop();
                            break;

                        case Op::MatrixPut:
                            interpolate_mtxf(&tmp_mtxf, &old_op.matrix_put.src, &new_op.matrix_put.src);
                            Matrix_Put(&tmp_mtxf);
                            break;

                        case Op::MatrixMult:
                            interpolate_mtxf(&tmp_mtxf, &old_op.matrix_mult.mf, &new_op.matrix_mult.mf);
                            Matrix_Mult(&tmp_mtxf, (MatrixMode)new_op.matrix_mult.mode);
                            break;

                        case Op::MatrixTranslate:
                            Matrix_Translate(lerp(old_op.matrix_translate.x, new_op.matrix_translate.x),
                                             lerp(old_op.matrix_translate.y, new_op.matrix_translate.y),
                                             lerp(old_op.matrix_translate.z, new_op.matrix_translate.z),
                                             (MatrixMode)new_op.matrix_translate.mode);
                            break;

                        case Op::MatrixScale:
                            Matrix_Scale(lerp(old_op.matrix_scale.x, new_op.matrix_scale.x),
                                         lerp(old_op.matrix_scale.y, new_op.matrix_scale.y),
                                         lerp(old_op.matrix_scale.z, new_op.matrix_scale.z),
                                         (MatrixMode)new_op.matrix_scale.mode);
                            break;

                        case Op::MatrixRotate1Coord: {
                            float v = interpolate_angle(old_op.matrix_rotate_1_coord.value,
                                                        new_op.matrix_rotate_1_coord.value);
                            u8 mode = new_op.matrix_rotate_1_coord.mode;
                            switch (new_op.matrix_rotate_1_coord.coord) {
                                case 0:
                                    Matrix_RotateXF(v, (MatrixMode)mode);
                                    break;

                                case 1:
                                    Matrix_RotateYF(v, (MatrixMode)mode);
                                    break;

                                case 2:
                                    Matrix_RotateZF(v, (MatrixMode)mode);
                                    break;
                            }
                            break;
                        }

                        case Op::MatrixRotateZYX:
                            Matrix_RotateZYX(interpolate_angle(old_op.matrix_rotate_zyx.x, new_op.matrix_rotate_zyx.x),
                                             interpolate_angle(old_op.matrix_rotate_zyx.y, new_op.matrix_rotate_zyx.y),
                                             interpolate_angle(old_op.matrix_rotate_zyx.z, new_op.matrix_rotate_zyx.z),
                                             (MatrixMode)new_op.matrix_rotate_zyx.mode);
                            break;

                        case Op::MatrixTranslateRotateZYX:
                            lerp_vec3f(&tmp_vec3f, &old_op.matrix_translate_rotate_zyx.translation,
                                       &new_op.matrix_translate_rotate_zyx.translation);
                            interpolate_angles(&tmp_vec3s, &old_op.matrix_translate_rotate_zyx.rotation,
                                               &new_op.matrix_translate_rotate_zyx.rotation);
                            Matrix_TranslateRotateZYX(&tmp_vec3f, &tmp_vec3s);
                            break;

                        case Op::MatrixSetTranslateRotateYXZ:
#ifdef LUS_XBOX
                            if (!new_op.matrix_set_translate_rotate_yxz.has_mtx ||
                                !old_op.matrix_set_translate_rotate_yxz.has_mtx) {
                                break; // (nothing reads it)
                            }
                            tmp_mtxf2 = *Matrix_GetCurrent(); // the game's stack left as it was
#endif
                            interpolate_wider_angles = new_op.matrix_set_translate_rotate_yxz.interpolate_wider_angles;
                            interpolate_angles(&tmp_vec3s, &old_op.matrix_set_translate_rotate_yxz.rot,
                                               &new_op.matrix_set_translate_rotate_yxz.rot);
                            Matrix_SetTranslateRotateYXZ(lerp(old_op.matrix_set_translate_rotate_yxz.translateX,
                                                              new_op.matrix_set_translate_rotate_yxz.translateX),
                                                         lerp(old_op.matrix_set_translate_rotate_yxz.translateY,
                                                              new_op.matrix_set_translate_rotate_yxz.translateY),
                                                         lerp(old_op.matrix_set_translate_rotate_yxz.translateZ,
                                                              new_op.matrix_set_translate_rotate_yxz.translateZ),
                                                         &tmp_vec3s);
                            if (new_op.matrix_set_translate_rotate_yxz.has_mtx &&
                                old_op.matrix_set_translate_rotate_yxz.has_mtx) {
                                actor_mtx = *Matrix_GetCurrent();
                            }
#ifdef LUS_XBOX
                            *Matrix_GetCurrent() = tmp_mtxf2;
#endif
                            interpolate_wider_angles = false;
                            break;

                        case Op::MatrixMtxFToMtx:
                            interpolate_mtxf(new_replacement(new_op.matrix_mtxf_to_mtx.dest),
                                             &old_op.matrix_mtxf_to_mtx.src, &new_op.matrix_mtxf_to_mtx.src);
                            break;

                        case Op::MatrixToMtx: {
                            //*new_replacement(new_op.matrix_to_mtx.dest) = *Matrix_GetCurrent();
                            if (self_paired && new_op.matrix_to_mtx.has_adjusted) {
                                *new_replacement(new_op.matrix_to_mtx.dest) = new_op.matrix_to_mtx.raw;
                            } else if (old_op.matrix_to_mtx.has_adjusted != new_op.matrix_to_mtx.has_adjusted) {
                                // has_adjusted toggled between frames (e.g. ActorShadow_Draw's isotropic-shadow
                                // check flipping as scale.x drifts in and out of equality with scale.z). old.src
                                // and new.src aren't comparable here: whichever side has has_adjusted=true holds
                                // an actor-relative matrix that still needs actor_mtx re-applied, not a final
                                // matrix. Use raw (the true un-relativized matrix for that frame) and snap to the
                                // new frame instead of interpolating.
                                *new_replacement(new_op.matrix_to_mtx.dest) = new_op.matrix_to_mtx.raw;
                            } else if (old_op.matrix_to_mtx.has_adjusted && new_op.matrix_to_mtx.has_adjusted) {
                                interpolate_mtxf(&tmp_mtxf, &old_op.matrix_to_mtx.src, &new_op.matrix_to_mtx.src);
                                SkinMatrix_MtxFMtxFMult(&actor_mtx, &tmp_mtxf,
                                                        new_replacement(new_op.matrix_to_mtx.dest));
                            } else {
                                interpolate_mtxf(new_replacement(new_op.matrix_to_mtx.dest), &old_op.matrix_to_mtx.src,
                                                 &new_op.matrix_to_mtx.src);
                            }
                            break;
                        }

                        case Op::MatrixReplaceRotation:
                            interpolate_mtxf(&tmp_mtxf, &old_op.matrix_replace_rotation.mf,
                                             &new_op.matrix_replace_rotation.mf);
                            Matrix_ReplaceRotation(&tmp_mtxf);
                            break;

                        case Op::MatrixRotateAxis:
                            lerp_vec3f(&tmp_vec3f, &old_op.matrix_rotate_axis.axis, &new_op.matrix_rotate_axis.axis);
                            Matrix_RotateAxisF(
                                interpolate_angle(old_op.matrix_rotate_axis.angle, new_op.matrix_rotate_axis.angle),
                                &tmp_vec3f, (MatrixMode)new_op.matrix_rotate_axis.mode);
                            break;

                        case Op::SkinMatrixMtxFToMtx:
                            break;
                    }
                }
            }
        }
    }
};

} // anonymous namespace

#ifdef LUS_XBOX
void FrameInterpolation_InterpolateInto(float step, MtxBlends& replacements) {
#else
void FrameInterpolation_InterpolateInto(float step, unordered_map<Mtx*, MtxF>& replacements) {
#endif
    InterpolateCtx ctx;
    ctx.step = step;
    ctx.w = 1.0f - step;
    ctx.mtx_replacements = &replacements;
    ctx.interpolate_branch(previous_recording, current_recording);
}

#ifdef LUS_XBOX
size_t FrameInterpolation_RecordingBytes(void) {
    return arenas[0].Bytes() + arenas[1].Bytes();
}
#else
unordered_map<Mtx*, MtxF> FrameInterpolation_Interpolate(float step) {
    unordered_map<Mtx*, MtxF> replacements;
    FrameInterpolation_InterpolateInto(step, replacements);
    return replacements;
}
#endif

bool camera_interpolation = false;

void FrameInterpolation_ShouldInterpolateFrame(bool shouldInterpolate) {
    camera_interpolation = shouldInterpolate;
}

void FrameInterpolation_StartRecord(void) {
    previous_recording = current_recording;
    current_recording = new_recording();
    current_path.clear();
    current_path.push_back(current_recording);
    has_inv_actor_mtx = false;
    interpolate_wider_angles = false;
    ignore_inv_actor_mtx = false;

    if (!camera_interpolation) {
        // default to interpolating
        camera_interpolation = true;
        is_recording = false;
        return;
    }
    if (OTRGlobals::Instance->GetInterpolationFPS() != 20) {
        is_recording = true;
    }
}

void FrameInterpolation_StopRecord(void) {
    previous_camera_epoch = camera_epoch;
    is_recording = false;
}

void FrameInterpolation_RecordOpenChild(const void* a, int b) {
    if (!is_recording)
        return;
    label key = { a, b };
    auto& m = current_path.back()->children[key];
    append(Op::OpenChild).open_child = { key, m.size() };
    current_path.push_back(&m.emplace_back());
}

void FrameInterpolation_RecordCloseChild(void) {
    if (!is_recording)
        return;
    // append(Op::CloseChild);
    if (has_inv_actor_mtx && current_path.size() == inv_actor_mtx_path_index) {
        has_inv_actor_mtx = false;
    }
    if (ignore_inv_actor_mtx && current_path.size() == ignore_inv_actor_mtx_path_index) {
        ignore_inv_actor_mtx = false;
    }
    current_path.pop_back();
}

void FrameInterpolation_DontInterpolateCamera(void) {
    camera_epoch = previous_camera_epoch + 1;
}

int FrameInterpolation_GetCameraEpoch(void) {
    return (int)camera_epoch;
}

// Marks the current record path and its children to not apply the matrix result
// against the recorded actor inverted matrix
void FrameInterpolation_IgnoreActorMtx() {
    if (!is_recording)
        return;
    ignore_inv_actor_mtx = true;
    ignore_inv_actor_mtx_path_index = current_path.size();
}

// Allows interpolating from angle changes that are up to 123º for the next SetTranslateRotateYXZ
void FrameInterpolation_InterpolateWiderAngles() {
    if (!is_recording)
        return;
    interpolate_wider_angles = true;
}

void FrameInterpolation_RecordActorPosRotMatrix(void) {
    if (!is_recording)
        return;
    next_is_actor_pos_rot_matrix = true;
}

void FrameInterpolation_RecordMatrixPush(void) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixPush);
}

void FrameInterpolation_RecordMatrixPop(void) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixPop);
}

void FrameInterpolation_RecordMatrixPut(MtxF* src) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixPut).matrix_put = { *src };
}

void FrameInterpolation_RecordMatrixMult(MtxF* mf, u8 mode) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixMult).matrix_mult = { *mf, mode };
}

void FrameInterpolation_RecordMatrixTranslate(f32 x, f32 y, f32 z, u8 mode) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixTranslate).matrix_translate = { x, y, z, mode };
}

void FrameInterpolation_RecordMatrixScale(f32 x, f32 y, f32 z, u8 mode) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixScale).matrix_scale = { x, y, z, mode };
}

void FrameInterpolation_RecordMatrixRotate1Coord(u32 coord, f32 value, u8 mode) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixRotate1Coord).matrix_rotate_1_coord = { coord, value, mode };
}

void FrameInterpolation_RecordMatrixRotateZYX(s16 x, s16 y, s16 z, u8 mode) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixRotateZYX).matrix_rotate_zyx = { x, y, z, mode };
}

void FrameInterpolation_RecordMatrixTranslateRotateZYX(Vec3f* translation, Vec3s* rotation) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixTranslateRotateZYX).matrix_translate_rotate_zyx = { *translation, *rotation };
}

void FrameInterpolation_RecordMatrixSetTranslateRotateYXZ(f32 translateX, f32 translateY, f32 translateZ, Vec3s* rot) {
    if (!is_recording)
        return;
    auto& d = append(Op::MatrixSetTranslateRotateYXZ).matrix_set_translate_rotate_yxz = { translateX, translateY,
                                                                                          translateZ, *rot };
    if (next_is_actor_pos_rot_matrix) {
        d.has_mtx = true;
        d.interpolate_wider_angles = interpolate_wider_angles;
        interpolate_wider_angles = false;
        // d.mtx = *Matrix_GetCurrent();
        invert_matrix((const float*)Matrix_GetCurrent()->mf, (float*)inv_actor_mtx.mf);
        next_is_actor_pos_rot_matrix = false;
        has_inv_actor_mtx = true;
        inv_actor_mtx_path_index = current_path.size();
    }
}

void FrameInterpolation_RecordMatrixMtxFToMtx(MtxF* src, Mtx* dest) {
    if (!is_recording)
        return;
    append(Op::MatrixMtxFToMtx).matrix_mtxf_to_mtx = { *src, dest };
}

void FrameInterpolation_RecordMatrixToMtx(Mtx* dest, char* file, s32 line) {
    if (!is_recording)
        return;
    auto& d = append(Op::MatrixToMtx).matrix_to_mtx = { dest };
    d.raw = *Matrix_GetCurrent();
    if (has_inv_actor_mtx && !ignore_inv_actor_mtx) {
        d.has_adjusted = true;
        SkinMatrix_MtxFMtxFMult(&inv_actor_mtx, Matrix_GetCurrent(), &d.src);
    } else {
        d.src = d.raw;
    }
}

void FrameInterpolation_RecordMatrixReplaceRotation(MtxF* mf) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixReplaceRotation).matrix_replace_rotation = { *mf };
}

void FrameInterpolation_RecordMatrixRotateAxis(f32 angle, Vec3f* axis, u8 mode) {
    if (!is_recording || !kRecordMatrixStack)
        return;
    append(Op::MatrixRotateAxis).matrix_rotate_axis = { angle, *axis, mode };
}

void FrameInterpolation_RecordSkinMatrixMtxFToMtx(MtxF* src, Mtx* dest) {
    if (!is_recording)
        return;
    FrameInterpolation_RecordMatrixMtxFToMtx(src, dest);
}

// https://stackoverflow.com/questions/1148309/inverting-a-4x4-matrix
static bool invert_matrix(const float m[16], float invOut[16]) {
    float inv[16], det;
    int i;

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0) {
        return false;
    }

    det = 1.0 / det;

    for (i = 0; i < 16; i++) {
        invOut[i] = inv[i] * det;
    }

    return true;
}
