#include "global.h"
#include "modding.h"
#include "overlays/actors/ovl_Door_Ana/z_door_ana.h"
#include "recompconfig.h"
#include "z64recomp_api.h"
#include "z64skin_matrix.h"

extern Gfx gameplay_field_keep_DL_000C40[];
extern u64 gBoulderFragmentsTex[];

typedef enum {
  OPTION_ON,
  OPTION_OFF,
} OnOffOption;

typedef enum {
  STYLE_EARTHEN,
  STYLE_SHADOWED,
  STYLE_VOID,
  STYLE_MAX,
} PitStyle;

typedef enum {
  PIT_VANILLA,
  PIT_REAL,
  PIT_FLATTENED,
} PitMode;

typedef enum {
  PASS_DEPTH_CLEAR,
  PASS_OPA,
  PASS_XLU,
} PitPass;

// world units at vanilla scale (0.01)
#define RIM_RADIUS 28.0f
#define SEGS 20
#define RINGS 6
static const f32 sRingDepthFrac[RINGS] = {0.0f, 0.06f, 0.15f,
                                          0.3f, 0.55f, 1.0f};

#define STYLE_STOPS 4
static const f32 sStyleStopDepth[STYLE_STOPS] = {0.0f, 0.1333f, 0.4167f, 1.0f};
static const u8 sStyleColors[STYLE_MAX][STYLE_STOPS][3] = {
    [STYLE_EARTHEN] = {{230, 215, 200},
                       {150, 135, 120},
                       {60, 52, 45},
                       {0, 0, 0}},
    [STYLE_SHADOWED] = {{140, 130, 120}, {80, 72, 64}, {25, 22, 19}, {0, 0, 0}},
    [STYLE_VOID] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
};

#define FLAT_LIFT 1.0f
#define MIN_EYE_ABOVE_FLAT_PLANE 1.0f

#define RIM_INNER_SCALE 0.98f
#define RIM_OUTER_SCALE 1.2f
// the rim is pulled toward the camera a bit because otherwise it z fights
#define RIM_DEPTH_BIAS 1.0f
#define RIM_MAX_PULL 50.0f
#define RIM_SAMPLE_HEIGHT 30.0f
#define RIM_MAX_HEIGHT_DIFF 15.0f
#define RIM_ALPHA 170

#define PIT_TEX gBoulderFragmentsTex
#define PIT_TEX_SIZE 32
#define TEXELS_PER_WORLD_UNIT 0.2f
#define MAX_DRAW_DIST 2500.0f

#define SWITCH_DELAY_FRAMES 4
#define VIEW_CHECK_MARGIN 1.05f
#define VIEW_CHECK_LIFT 3.0f

#define VTX_SCALE 10.0f

#define RING_VTX (SEGS + 1)
#define SEGS_PER_LOAD 10
#define MAX_POLY_VERTS (3 + SEGS)
#define MAX_OUT_VTX 1536
#define VTX_PER_LOAD 30

typedef struct {
  Mtx *mtx;
  u8 mode;
  u8 clearFrames;
  f32 rimSampleWidth;
  f32 rimInnerY[SEGS];
  f32 rimOuterY[SEGS];
} GrottoData;

typedef struct {
  Vec3f pos;
  f32 s, t;
  f32 r, g, b;
} PitVert;

typedef struct {
  f32 depth;
  f32 width;
  u32 style;
  u8 rim;
} PitConfig;

typedef struct {
  PitVert walls[RINGS][RING_VTX];
  PitVert floor[RING_VTX];
  PitVert floorCenter;
  f32 maxRadius;
  Vtx *wallVtx;
  Vtx *floorVtx;
  Vtx *floorCenterVtx;
  Vtx *openingCenterVtx;
} PitShape;

static PitConfig sConfig;
static u8 sFrameReady;
static Vec3f sFlatEye;
static f32 sFlatPlaneY;
static s32 sFlatCount;

static PitShape sRealShape;
static PitShape sFlatShape;
static PitVert sFlatRim[SEGS];
static Vtx sOutVtx[MAX_OUT_VTX];
static PitVert sClipBufA[MAX_POLY_VERTS];
static PitVert sClipBufB[MAX_POLY_VERTS];

static ActorExtensionId sGrottoExt;

RECOMP_CALLBACK("*", recomp_on_init) void Grottos_Init(void) {
  sGrottoExt = z64recomp_extend_actor(ACTOR_DOOR_ANA, sizeof(GrottoData));
}

static f32 ConfigNumber(const char *key, f32 min, f32 max) {
  f32 value = recomp_get_config_double(key);

  return CLAMP(value, min, max);
}

static void ReadConfig(PitConfig *config) {
  config->depth = ConfigNumber("pit_depth", 20.0f, 150.0f);
  config->width = ConfigNumber("pit_width", 80.0f, 120.0f) / 100.0f;
  config->style = recomp_get_config_u32("pit_style");
  if (config->style >= STYLE_MAX) {
    config->style = STYLE_EARTHEN;
  }
  config->rim = (recomp_get_config_u32("rim_blend") == OPTION_ON);
}

static f32 RimWobble(s32 seg, s32 jagged) {
  s16 angle;

  if (jagged) {
    return 1.0f + 0.03f * Math_SinS((s16)(seg * 0x4580)) +
           0.02f * Math_SinS((s16)(seg * 0x8C30 + 0x2000));
  }
  angle = (s16)(seg * (0x10000 / SEGS));
  return 1.0f + 0.03f * Math_CosS(angle * 2) +
         0.015f * Math_SinS(angle * 3 + 0x1800);
}

static void SetColor(PitVert *v, f32 depthFrac) {
  const u8(*colors)[3] = sStyleColors[sConfig.style];
  s32 i = 0;
  f32 t;

  while ((i < STYLE_STOPS - 2) && (depthFrac > sStyleStopDepth[i + 1])) {
    i++;
  }
  t = CLAMP(
      F32_LERPWEIGHT(depthFrac, sStyleStopDepth[i], sStyleStopDepth[i + 1]),
      0.0f, 1.0f);
  v->r = F32_LERPIMP(colors[i][0], colors[i + 1][0], t);
  v->g = F32_LERPIMP(colors[i][1], colors[i + 1][1], t);
  v->b = F32_LERPIMP(colors[i][2], colors[i + 1][2], t);
}

static void SetPlanarUV(PitVert *v) {
  v->s = v->pos.x * TEXELS_PER_WORLD_UNIT;
  v->t = v->pos.z * TEXELS_PER_WORLD_UNIT;
}

static void SetVtx(Vtx *v, const PitVert *p, f32 y, u8 alpha) {
  v->v.ob[0] = (s16)(p->pos.x * VTX_SCALE);
  v->v.ob[1] = (s16)(y * VTX_SCALE);
  v->v.ob[2] = (s16)(p->pos.z * VTX_SCALE);
  v->v.flag = 0;
  v->v.tc[0] = (s16)(p->s * 32.0f);
  v->v.tc[1] = (s16)(p->t * 32.0f);
  v->v.cn[0] = (u8)p->r;
  v->v.cn[1] = (u8)p->g;
  v->v.cn[2] = (u8)p->b;
  v->v.cn[3] = alpha;
}

static void BuildPitShape(GraphicsContext *gfxCtx, PitShape *shape,
                          s32 jagged) {
  f32 radius = RIM_RADIUS * sConfig.width;
  f32 wallRepeats = MAX(
      1, (s32)(2.0f * M_PIf * radius * TEXELS_PER_WORLD_UNIT / PIT_TEX_SIZE +
               0.5f));

  shape->maxRadius = 0.0f;
  for (s32 ring = 0; ring < RINGS; ring++) {
    f32 d = sRingDepthFrac[ring];
    f32 ringRadius = radius * (1.0f - 0.14f * d * sqrtf(d));

    for (s32 i = 0; i < RING_VTX; i++) {
      s32 seg = i % SEGS;
      s16 angle = (s16)(seg * (0x10000 / SEGS));
      f32 r = ringRadius * RimWobble(seg, jagged);
      PitVert *v = &shape->walls[ring][i];

      v->pos.x = r * Math_CosS(angle);
      v->pos.y = -sConfig.depth * d;
      v->pos.z = r * Math_SinS(angle);
      v->s = (f32)i / SEGS * wallRepeats * PIT_TEX_SIZE;
      v->t = sConfig.depth * d * TEXELS_PER_WORLD_UNIT;
      SetColor(v, d);
      shape->maxRadius = MAX(shape->maxRadius, r);
    }
  }
  for (s32 i = 0; i < RING_VTX; i++) {
    shape->floor[i] = shape->walls[RINGS - 1][i];
    SetPlanarUV(&shape->floor[i]);
  }
  shape->floorCenter = shape->floor[0];
  shape->floorCenter.pos.x = shape->floorCenter.pos.z = 0.0f;
  SetPlanarUV(&shape->floorCenter);

  shape->wallVtx = GRAPH_ALLOC(gfxCtx, sizeof(Vtx) * RINGS * RING_VTX);
  shape->floorVtx = GRAPH_ALLOC(gfxCtx, sizeof(Vtx) * RING_VTX);
  shape->floorCenterVtx = GRAPH_ALLOC(gfxCtx, sizeof(Vtx));
  shape->openingCenterVtx = GRAPH_ALLOC(gfxCtx, sizeof(Vtx));
  for (s32 ring = 0; ring < RINGS; ring++) {
    for (s32 i = 0; i < RING_VTX; i++) {
      SetVtx(&shape->wallVtx[ring * RING_VTX + i], &shape->walls[ring][i],
             shape->walls[ring][i].pos.y, 255);
    }
  }
  for (s32 i = 0; i < RING_VTX; i++) {
    SetVtx(&shape->floorVtx[i], &shape->floor[i], shape->floor[i].pos.y, 255);
  }
  SetVtx(shape->floorCenterVtx, &shape->floorCenter, shape->floorCenter.pos.y,
         255);
  SetVtx(shape->openingCenterVtx, &shape->floorCenter, 0.0f, 255);
}

static Gfx *LoadPitTexture(Gfx *gfx) {
  gDPLoadTextureBlock(gfx++, PIT_TEX, G_IM_FMT_RGBA, G_IM_SIZ_16b, PIT_TEX_SIZE,
                      PIT_TEX_SIZE, 0, G_TX_WRAP | G_TX_NOMIRROR,
                      G_TX_WRAP | G_TX_NOMIRROR, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
  gSPTexture(gfx++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
  return gfx;
}

#define PIT_OTHERMODE_H(cycleType)                                             \
  (G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP |         \
   G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | (cycleType) |             \
   G_PM_NPRIMITIVE)

static Gfx *SetupPitDraw(Gfx *gfx, Mtx *mtx, PitPass pass) {
  gDPPipeSync(gfx++);
  gSPMatrix(gfx++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
  gSPClearGeometryMode(gfx++, G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN |
                                  G_TEXTURE_GEN_LINEAR | G_FOG);
  gSPSetGeometryMode(gfx++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);

  switch (pass) {
  case PASS_DEPTH_CLEAR:
    gSPTexture(gfx++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetOtherMode(gfx++, PIT_OTHERMODE_H(G_CYC_1CYCLE),
                    G_AC_NONE | G_ZS_PRIM | (G_RM_OPA_SURF | Z_UPD) |
                        (G_RM_OPA_SURF2 | Z_UPD));
    gDPSetCombineMode(gfx++, G_CC_SHADE, G_CC_SHADE);
    gDPSetPrimDepth(gfx++, 0x7FFF, 0);
    break;

  case PASS_OPA:
    gfx = LoadPitTexture(gfx);
    gDPSetOtherMode(gfx++, PIT_OTHERMODE_H(G_CYC_1CYCLE),
                    G_AC_NONE | G_ZS_PIXEL | G_RM_ZB_OPA_SURF |
                        G_RM_ZB_OPA_SURF2);
    gDPSetCombineMode(gfx++, G_CC_MODULATEI, G_CC_MODULATEI);
    break;

  case PASS_XLU:
    gfx = LoadPitTexture(gfx);
    gDPSetOtherMode(gfx++, PIT_OTHERMODE_H(G_CYC_1CYCLE),
                    G_AC_NONE | G_ZS_PIXEL | G_RM_ZB_XLU_SURF |
                        G_RM_ZB_XLU_SURF2);
    gDPSetCombineMode(gfx++, G_CC_MODULATEI, G_CC_MODULATEI);
    break;
  }
  return gfx;
}

static Gfx *DrawTriangles(Gfx *gfx, Vtx *vtx, s32 count) {
  for (s32 start = 0; start < count; start += VTX_PER_LOAD) {
    s32 n = MIN(VTX_PER_LOAD, count - start);

    gSPVertex(gfx++, &vtx[start], n, 0);
    for (s32 i = 0; i + 5 < n; i += 6) {
      gSP2Triangles(gfx++, i, i + 1, i + 2, 0, i + 3, i + 4, i + 5, 0);
    }
    if (n % 6 != 0) {
      gSP1Triangle(gfx++, n - 3, n - 2, n - 1, 0);
    }
  }
  return gfx;
}

static Gfx *DrawDisc(Gfx *gfx, Vtx *ring, Vtx *center) {
  gSPVertex(gfx++, ring, RING_VTX, 0);
  gSPVertex(gfx++, center, 1, RING_VTX);
  for (s32 i = 0; i < SEGS; i += 2) {
    gSP2Triangles(gfx++, RING_VTX, i + 1, i, 0, RING_VTX, i + 2, i + 1, 0);
  }
  return gfx;
}

static Gfx *DrawBand(Gfx *gfx, Vtx *top, Vtx *bottom) {
  for (s32 start = 0; start < SEGS; start += SEGS_PER_LOAD) {
    gSPVertex(gfx++, &top[start], SEGS_PER_LOAD + 1, 0);
    gSPVertex(gfx++, &bottom[start], SEGS_PER_LOAD + 1, SEGS_PER_LOAD + 1);
    for (s32 i = 0; i < SEGS_PER_LOAD; i++) {
      s32 t = i;
      s32 b = SEGS_PER_LOAD + 1 + i;

      gSP2Triangles(gfx++, t, b, t + 1, 0, t + 1, b, b + 1, 0);
    }
  }
  return gfx;
}

static PitVert Flatten(const PitVert *q) {
  PitVert p = *q;
  f32 t = (sFlatEye.y - sFlatPlaneY) / (sFlatEye.y - q->pos.y);

  Math_Vec3f_Lerp(&sFlatEye, (Vec3f *)&q->pos, t, &p.pos);
  return p;
}

static PitVert LerpVert(const PitVert *a, const PitVert *b, f32 t) {
  PitVert v;

  Math_Vec3f_Lerp((Vec3f *)&a->pos, (Vec3f *)&b->pos, t, &v.pos);
  v.s = F32_LERPIMP(a->s, b->s, t);
  v.t = F32_LERPIMP(a->t, b->t, t);
  v.r = F32_LERPIMP(a->r, b->r, t);
  v.g = F32_LERPIMP(a->g, b->g, t);
  v.b = F32_LERPIMP(a->b, b->b, t);
  return v;
}

static f32 InsideEdge(const PitVert *a, const PitVert *b, const PitVert *p) {
  return (b->pos.x - a->pos.x) * (p->pos.z - a->pos.z) -
         (b->pos.z - a->pos.z) * (p->pos.x - a->pos.x);
}

static void EmitClipped(const PitVert *tri) {
  PitVert *in = sClipBufA;
  PitVert *out = sClipBufB;
  s32 count = 3;

  for (s32 i = 0; i < 3; i++) {
    in[i] = tri[i];
  }
  for (s32 e = 0; (e < SEGS) && (count >= 3); e++) {
    const PitVert *a = &sFlatRim[e];
    const PitVert *b = &sFlatRim[(e + 1) % SEGS];
    s32 outCount = 0;
    PitVert *swap;

    for (s32 i = 0; i < count; i++) {
      const PitVert *cur = &in[i];
      const PitVert *next = &in[(i + 1) % count];
      f32 dCur = InsideEdge(a, b, cur);
      f32 dNext = InsideEdge(a, b, next);

      if (dCur >= 0.0f) {
        out[outCount++] = *cur;
      }
      if ((dCur >= 0.0f) != (dNext >= 0.0f)) {
        out[outCount++] = LerpVert(cur, next, dCur / (dCur - dNext));
      }
    }
    count = outCount;
    swap = in;
    in = out;
    out = swap;
  }

  for (s32 i = 1; (i + 1 < count) && (sFlatCount + 3 <= MAX_OUT_VTX); i++) {
    SetVtx(&sOutVtx[sFlatCount++], &in[0], sFlatPlaneY, 255);
    SetVtx(&sOutVtx[sFlatCount++], &in[i], sFlatPlaneY, 255);
    SetVtx(&sOutVtx[sFlatCount++], &in[i + 1], sFlatPlaneY, 255);
  }
}

static void AddFlatTriangle(const PitVert *a, const PitVert *b,
                            const PitVert *c) {
  PitVert flat[3];
  Vec3f normal;
  Vec3f toEye;

  Math3D_SurfaceNorm((Vec3f *)&a->pos, (Vec3f *)&b->pos, (Vec3f *)&c->pos,
                     &normal);
  Math_Vec3f_Diff(&sFlatEye, (Vec3f *)&a->pos, &toEye);
  if (DOTXYZ(normal, toEye) <= 0.0f) {
    return;
  }
  flat[0] = Flatten(a);
  flat[1] = Flatten(b);
  flat[2] = Flatten(c);
  EmitClipped(flat);
}

// we draw a flattened pit (a vertex projection) when the Door_Ana obj is behind
// geometry like a tree or something this fallback stops it from rendering above
// geometry in the way, but it doesnt look great up-close and link falls through
// the plane
static void DrawFlattenedPit(PlayState *play, Actor *actor, Mtx *mtx) {
  const PitShape *shape = &sFlatShape;
  f32 scale = actor->scale.x / 0.01f;
  Vtx *vtx;

  Math_Vec3f_Diff(&play->view.eye, &actor->world.pos, &sFlatEye);
  Math_Vec3f_Scale(&sFlatEye, 1.0f / scale);
  sFlatPlaneY = FLAT_LIFT / scale;
  sFlatCount = 0;

  for (s32 i = 0; i < SEGS; i++) {
    sFlatRim[i] = Flatten(&shape->walls[0][i]);
  }
  for (s32 ring = 0; ring < RINGS - 1; ring++) {
    for (s32 i = 0; i < SEGS; i++) {
      const PitVert *t0 = &shape->walls[ring][i];
      const PitVert *t1 = &shape->walls[ring][i + 1];
      const PitVert *b0 = &shape->walls[ring + 1][i];
      const PitVert *b1 = &shape->walls[ring + 1][i + 1];

      AddFlatTriangle(t0, b0, t1);
      AddFlatTriangle(t1, b0, b1);
    }
  }
  for (s32 i = 0; i < SEGS; i++) {
    AddFlatTriangle(&shape->floorCenter, &shape->floor[i + 1],
                    &shape->floor[i]);
  }

  if (sFlatCount == 0) {
    return;
  }
  vtx = GRAPH_ALLOC(play->state.gfxCtx, sizeof(Vtx) * sFlatCount);
  Lib_MemCpy(vtx, sOutVtx, sizeof(Vtx) * sFlatCount);

  OPEN_DISPS(play->state.gfxCtx);
  POLY_OPA_DISP = SetupPitDraw(POLY_OPA_DISP, mtx, PASS_OPA);
  POLY_OPA_DISP = DrawTriangles(POLY_OPA_DISP, vtx, sFlatCount);
  CLOSE_DISPS(play->state.gfxCtx);
}

static void DrawRealPit(PlayState *play, Mtx *mtx) {
  const PitShape *shape = &sRealShape;

  OPEN_DISPS(play->state.gfxCtx);
  POLY_OPA_DISP = SetupPitDraw(POLY_OPA_DISP, mtx, PASS_DEPTH_CLEAR);
  POLY_OPA_DISP =
      DrawDisc(POLY_OPA_DISP, shape->wallVtx, shape->openingCenterVtx);
  POLY_OPA_DISP = SetupPitDraw(POLY_OPA_DISP, mtx, PASS_OPA);
  for (s32 ring = 0; ring < RINGS - 1; ring++) {
    POLY_OPA_DISP = DrawBand(POLY_OPA_DISP, &shape->wallVtx[ring * RING_VTX],
                             &shape->wallVtx[(ring + 1) * RING_VTX]);
  }
  POLY_OPA_DISP =
      DrawDisc(POLY_OPA_DISP, shape->floorVtx, shape->floorCenterVtx);
  CLOSE_DISPS(play->state.gfxCtx);
}

static f32 GroundHeightAt(PlayState *play, Actor *actor, s16 angle,
                          f32 radius) {
  CollisionPoly *poly;
  Vec3f pos;
  f32 floorY;

  pos.x = actor->world.pos.x + radius * Math_CosS(angle);
  pos.y = actor->world.pos.y + RIM_SAMPLE_HEIGHT;
  pos.z = actor->world.pos.z + radius * Math_SinS(angle);
  floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &poly, &pos) -
           actor->world.pos.y;

  return (fabsf(floorY) > RIM_MAX_HEIGHT_DIFF) ? 0.0f : floorY;
}

static void SampleRimHeights(PlayState *play, Actor *actor, GrottoData *data) {
  f32 radius = RIM_RADIUS * sConfig.width;

  if (data->rimSampleWidth == sConfig.width) {
    return;
  }
  for (s32 i = 0; i < SEGS; i++) {
    s16 angle = (s16)(i * (0x10000 / SEGS));

    data->rimInnerY[i] =
        GroundHeightAt(play, actor, angle, radius * RIM_INNER_SCALE);
    data->rimOuterY[i] =
        GroundHeightAt(play, actor, angle, radius * RIM_OUTER_SCALE);
  }
  data->rimSampleWidth = sConfig.width;
}

static void SetRimVtx(Vtx *v, PitVert p, f32 groundY, const Vec3f *eye,
                      f32 scale, u8 alpha) {
  Vec3f world = {p.pos.x * scale, groundY, p.pos.z * scale};
  Vec3f toEye;
  f32 dist;

  SetPlanarUV(&p);
  Math_Vec3f_Diff((Vec3f *)eye, &world, &toEye);
  dist = sqrtf(SQXYZ(toEye));
  if (toEye.y > 0.0f) {
    f32 pull =
        MIN(MIN(RIM_DEPTH_BIAS * dist / toEye.y, RIM_MAX_PULL), dist * 0.9f);

    Math_Vec3f_Scale(&toEye, pull / dist);
    Math_Vec3f_Sum(&world, &toEye, &world);
  }
  p.pos.x = world.x / scale;
  p.pos.z = world.z / scale;
  SetVtx(v, &p, world.y / scale, alpha);
}

static void DrawRim(PlayState *play, Actor *actor, GrottoData *data,
                    const PitShape *shape) {
  f32 scale = actor->scale.x / 0.01f;
  Vtx *inner = GRAPH_ALLOC(play->state.gfxCtx, sizeof(Vtx) * RING_VTX);
  Vtx *outer = GRAPH_ALLOC(play->state.gfxCtx, sizeof(Vtx) * RING_VTX);
  Vec3f eye;

  SampleRimHeights(play, actor, data);
  Math_Vec3f_Diff(&play->view.eye, &actor->world.pos, &eye);
  for (s32 i = 0; i < RING_VTX; i++) {
    s32 seg = i % SEGS;
    PitVert in = shape->walls[0][i];
    PitVert out = in;

    in.pos.x *= RIM_INNER_SCALE;
    in.pos.z *= RIM_INNER_SCALE;
    out.pos.x *= RIM_OUTER_SCALE;
    out.pos.z *= RIM_OUTER_SCALE;
    SetRimVtx(&inner[i], in, data->rimInnerY[seg], &eye, scale, RIM_ALPHA);
    SetRimVtx(&outer[i], out, data->rimOuterY[seg], &eye, scale, 0);
  }

  OPEN_DISPS(play->state.gfxCtx);
  POLY_XLU_DISP = SetupPitDraw(POLY_XLU_DISP, data->mtx, PASS_XLU);
  POLY_XLU_DISP = DrawBand(POLY_XLU_DISP, inner, outer);
  CLOSE_DISPS(play->state.gfxCtx);
}

static Mtx *GrottoMatrix(PlayState *play, Actor *actor) {
  f32 scale = actor->scale.x / 0.01f / VTX_SCALE;
  Mtx *mtx;

  Matrix_Push();
  Matrix_Translate(actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                   MTXMODE_NEW);
  Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
  mtx = Matrix_Finalize(play->state.gfxCtx);
  Matrix_Pop();
  return mtx;
}

static s32 HasClearView(PlayState *play, Actor *actor) {
  Vec3f eye = play->view.eye;
  f32 radius =
      sRealShape.maxRadius * (actor->scale.x / 0.01f) * VIEW_CHECK_MARGIN;
  f32 dx;
  f32 dz;
  f32 len = Math_Vec3f_DistXZAndStore(&eye, &actor->world.pos, &dx, &dz);

  if (len < 1.0f) {
    return true;
  }
  for (s32 side = -1; side <= 1; side += 2) {
    Vec3f target = actor->world.pos;
    Vec3f hit;
    CollisionPoly *poly;
    s32 bgId;

    target.x += -dz / len * radius * side;
    target.z += dx / len * radius * side;
    target.y += VIEW_CHECK_LIFT;
    if (BgCheck_EntityLineTest1(&play->colCtx, &eye, &target, &hit, &poly, true,
                                true, true, true, &bgId)) {
      return false;
    }
  }
  return true;
}

// a blocked view switches the grotto to the flattened version immediately, but
// waits a few frames to return to the full 3d versin
static s32 UpdateClearView(GrottoData *data, s32 clearNow) {
  data->clearFrames =
      clearNow ? MIN(data->clearFrames + 1, SWITCH_DELAY_FRAMES) : 0;
  return data->clearFrames >= SWITCH_DELAY_FRAMES;
}

static s32 IsBehindCamera(PlayState *play, Actor *actor) {
  Vec3f projected;
  f32 w;

  SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &actor->world.pos,
                               &projected, &w);
  return w < -RIM_RADIUS * RIM_OUTER_SCALE;
}

RECOMP_HOOK("Scene_Draw") void Grottos_BeforeSceneDraw(PlayState *play) {
  s32 enabled = (recomp_get_config_u32("enabled") == OPTION_ON);

  sFrameReady = false;
  for (Actor *actor = play->actorCtx.actorLists[ACTORCAT_ITEMACTION].first;
       actor != NULL; actor = actor->next) {
    GrottoData *data;

    if (actor->id != ACTOR_DOOR_ANA) {
      continue;
    }
    data = z64recomp_get_extended_actor_data(actor, sGrottoExt);
    data->mode = PIT_VANILLA;

    if (!enabled || (actor->scale.x < 0.0001f) ||
        (Math_Vec3f_DistXYZ(&play->view.eye, &actor->world.pos) >
         MAX_DRAW_DIST) ||
        (play->view.eye.y <
         actor->world.pos.y + FLAT_LIFT + MIN_EYE_ABOVE_FLAT_PLANE) ||
        IsBehindCamera(play, actor)) {
      continue;
    }
    if (!sFrameReady) {
      ReadConfig(&sConfig);
      BuildPitShape(play->state.gfxCtx, &sRealShape, true);
      BuildPitShape(play->state.gfxCtx, &sFlatShape, false);
      sFrameReady = true;
    }
    data->mtx = GrottoMatrix(play, actor);
    if (UpdateClearView(data, HasClearView(play, actor))) {
      data->mode = PIT_REAL;
    } else {
      data->mode = PIT_FLATTENED;
      DrawFlattenedPit(play, actor, data->mtx);
    }
  }
}

RECOMP_HOOK("Actor_DrawAll")
void Grottos_BeforeActorDraw(PlayState *play, ActorContext *actorCtx) {
  if (!sFrameReady) {
    return;
  }
  for (Actor *actor = actorCtx->actorLists[ACTORCAT_ITEMACTION].first;
       actor != NULL; actor = actor->next) {
    GrottoData *data;

    if (actor->id != ACTOR_DOOR_ANA) {
      continue;
    }
    data = z64recomp_get_extended_actor_data(actor, sGrottoExt);
    if (data->mode == PIT_REAL) {
      DrawRealPit(play, data->mtx);
    }
    if ((data->mode != PIT_VANILLA) && sConfig.rim) {
      DrawRim(play, actor, data,
              (data->mode == PIT_REAL) ? &sRealShape : &sFlatShape);
    }
  }
}

RECOMP_PATCH void DoorAna_Draw(Actor *thisx, PlayState *play) {
  GrottoData *data = z64recomp_get_extended_actor_data(thisx, sGrottoExt);

  if (data->mode == PIT_VANILLA) {
    Gfx_DrawDListXlu(play, gameplay_field_keep_DL_000C40);
  }
}
