/*
 * physics_ext.cpp — Nova Engine Physics Extension
 *
 * Implements all declarations in physics_ext.hpp.
 * Add to your build alongside physics.cpp; no changes needed to existing files.
 *
 * Build note: link order: physics.cpp, physics_ext.cpp (ext uses internal statics
 * via the public API, so no ODR issues).
 */

#define _USE_MATH_DEFINES
#include <cmath>
#include <random>
#include "physics_ext.hpp"

namespace phys {

// ════════════════════════════════════════════════════════════════════════════
//  INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════
static Vec2 s_gravity_dir   = {0.f, 1.f};  // default: downward in pixel-space
static float s_gravity_mag  = GRAVITY;       // pixels/s²

static std::vector<Manifold> s_last_manifolds;

static std::function<void(Entity* joint_entity, float break_force)> s_joint_break_cb;

static std::mt19937 s_rng{std::random_device{}()};

// Pointer to active entity list — set by apply_physics_ext so threshold setters
// can broadcast to all bodies (㉛).
EntityList* s_active_entities_ext = nullptr;

// FORWARD DECLARATIONS for functions defined later in this file
// ════════════════════════════════════════════════════════════════════════════
static void rebuild_contact_index(std::vector<Manifold>& manifolds);
static void dispatch_stay_callbacks();
static bool s_magnus_enabled = false;
static bool effector_affects_layer(Entity& effector_entity, const std::string& effector_type, int body_layer);
static PhysicsMaterial read_material(const Entity& col);
static bool poly_is_convex(const Verts& v);
static void decompose_convex(const Verts& poly, std::vector<Verts>& out, int depth=0);
static std::pair<int,int> find_best_diagonal(const Verts& poly);
static std::pair<Vec2,float> closest_on_poly(float px,float py,const Verts& v);
static std::optional<Manifold> poly_vs_circle(const Shape& poly, const Shape& circ);
static std::optional<Manifold> poly_vs_poly(const Shape& A, const Shape& B);
static std::optional<Manifold> capsule_vs_capsule(const Shape& A, const Shape& B);
static std::optional<Manifold> capsule_vs_circle(const Shape& cap, const Shape& circ);
static std::optional<Manifold> capsule_vs_poly(const Shape& cap, const Shape& poly);

// ════════════════════════════════════════════════════════════════════════════
//  INTERNAL HELPERS (mirrored from physics.cpp — static linkage requires copies)
// ════════════════════════════════════════════════════════════════════════════
static std::string body_type(const Entity& rb) {
    if (rb.is_null()) return "static";
    if (rb.value("is_kinematic",false)) return "kinematic";
    std::string bt = rb.value("body_type","dynamic");
    if (bt=="static"||bt=="kinematic"||bt=="dynamic") return bt;
    return "dynamic";
}

// ── Missing helpers (dependency order) ───────────────────────────────────
static bool segs_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    auto cr=[](Vec2 o,Vec2 p,Vec2 q){return (p.first-o.first)*(q.second-o.second)-(p.second-o.second)*(q.first-o.first);};
    float d1=cr(c,d,a),d2=cr(c,d,b),d3=cr(a,b,c),d4=cr(a,b,d);
    if(((d1>0&&d2<0)||(d1<0&&d2>0))&&((d3>0&&d4<0)||(d3<0&&d4>0))) return true;
    return false;
}
static bool diagonal_inside(const Verts& poly, int i, int j) {
    int n=(int)poly.size();
    Vec2 a=poly[i],b=poly[j];
    for(int k=0;k<n;++k){
        int k2=(k+1)%n;
        if(k==i||k==j||k2==i||k2==j) continue;
        if(segs_intersect(a,b,poly[k],poly[k2])) return false;
    }
    return true;
}
static bool is_convex_vertex(const Verts& poly, int i) {
    int n=(int)poly.size();
    Vec2 prev=poly[(i+n-1)%n],cur=poly[i],next=poly[(i+1)%n];
    float cross=(cur.first-prev.first)*(next.second-prev.second)-(cur.second-prev.second)*(next.first-prev.first);
    return cross>=0.f;
}

// Find the best diagonal to split a concave polygon.
// Returns (i,j) indices or (-1,-1) if polygon is already convex.
static std::pair<int,int> find_best_diagonal(const Verts& poly) {
    int n=(int)poly.size();
    for(int i=0;i<n;++i){
        if(is_convex_vertex(poly,i)) continue; // only from reflex vertices
        for(int j=0;j<n;++j){
            if(std::abs(i-j)<=1||std::abs(i-j)==n-1) continue;
            if(diagonal_inside(poly,i,j)) return {i,j};
        }
    }
    return {-1,-1};
}
struct SATResult { float pen; Vec2 axis; int ref_edge_idx; bool from_a; };
static std::optional<SATResult> sat_one_way(const Verts& va, const Verts& vb, bool from_a) {
    float best=1e30f; int best_i=-1; Vec2 baxis{0,1};
    int n=(int)va.size();
    for (int i=0;i<n;++i){
        auto [ax,ay]=va[i]; auto [bx,by]=va[(i+1)%n];
        auto [nx,ny]=normalize(by-ay,ax-bx);
        float mn=-1e30f;
        for (auto [px,py]:va) mn=std::max(mn,dot(px,py,nx,ny));
        float mn2=1e30f;
        for (auto [px,py]:vb) mn2=std::min(mn2,dot(px,py,nx,ny));
        float ov=mn-mn2;
        if (ov<0) return std::nullopt;
        if (ov<best){best=ov;baxis={nx,ny};best_i=i;}
    }
    return SATResult{best,baxis,best_i,from_a};
}
static int clip_segment_to_plane(Vec2 v0, Vec2 v1, float nx, float ny, float d,
                                  Vec2 out[2]) {
    float d0 = dot(v0.first,v0.second,nx,ny) - d;
    float d1 = dot(v1.first,v1.second,nx,ny) - d;
    int cnt=0;
    if (d0>=0) out[cnt++]=v0;
    if (d1>=0) out[cnt++]=v1;
    if ((d0>0&&d1<0)||(d0<0&&d1>0)){
        float t=d0/(d0-d1);
        out[cnt++]={v0.first+(v1.first-v0.first)*t, v0.second+(v1.second-v0.second)*t};
    }
    return std::min(cnt,2);
}

static std::vector<Shape> collect_shapes(Entity& e) {
    std::vector<Shape> out;

    // ── PolygonCollider2D: decompose concave polygons into convex pieces ──────
    // Unity auto-decomposes; we now match that behaviour.
    if (has_component(e,"Transform") && has_component(e,"components") && e["components"].contains("PolygonCollider2D")) {
        auto& poly = e["components"]["PolygonCollider2D"];
        auto pts = poly.value("points", Entity::array());
        if (pts.size() >= 3) {
            auto wt = transform::cached_world(e);
            float tx=finite_val(wt.x), ty=finite_val(wt.y);
            float rot=wt.rotation*(float)M_PI/180.f;
            float c=std::cos(rot), s=std::sin(rot);
            float ox=finite_val(get_float(poly,"offset_x")), oy=finite_val(get_float(poly,"offset_y"));
            Verts local_verts;
            for (auto& p : pts)
                local_verts.push_back({finite_val((float)p[0])+ox, finite_val((float)p[1])+oy});
            local_verts = ensure_ccw(local_verts);

            if (poly_is_convex(local_verts)) {
                // Fast path: already convex, transform directly
                Verts world_verts;
                for (auto [lx,ly] : local_verts)
                    world_verts.push_back(world_from_local(tx,ty,c,s,lx,ly));
                PhysicsMaterial mat = read_material(poly);
                Shape sh; sh.entity=&e; sh.col=&poly; sh.sub_id=0;
                sh.material=mat; sh.kind=ShapeKind::Polygon;
                sh.verts=ensure_ccw(world_verts); sh.cx=tx; sh.cy=ty;
                out.push_back(std::move(sh));
            } else {
                // Concave: decompose into convex pieces
                std::vector<Verts> pieces;
                decompose_convex(local_verts, pieces);
                PhysicsMaterial mat = read_material(poly);
                for (int pi=0; pi<(int)pieces.size(); ++pi) {
                    Verts world_verts;
                    for (auto [lx,ly] : pieces[pi])
                        world_verts.push_back(world_from_local(tx,ty,c,s,lx,ly));
                    Shape sh; sh.entity=&e; sh.col=&poly; sh.sub_id=pi;
                    sh.material=mat; sh.kind=ShapeKind::Polygon;
                    sh.verts=ensure_ccw(world_verts); sh.cx=tx; sh.cy=ty;
                    out.push_back(std::move(sh));
                }
            }
            // Skip build_shape's PolygonCollider2D branch if we generated shapes
            if (!out.empty()) goto skip_polygon_build_shape;
        }
    }

    // Primary collider (non-polygon handled above)
    if (auto sh=build_shape(e)) out.push_back(*sh);

    skip_polygon_build_shape:;

    // Additional colliders (composite support)
    if (has_component(e,"CompositeCollider2D")){
        auto& comp=e["components"]["CompositeCollider2D"];
        auto colliders=comp.value("colliders",Entity::array());
        for (auto& col_ref:colliders){
            if (col_ref.contains("type")&&col_ref.contains("offset_x")&&col_ref.contains("offset_y")){
                std::string type = col_ref.value("type", std::string{});
                float ox = finite_val(col_ref.value("offset_x", 0.f));
                float oy = finite_val(col_ref.value("offset_y", 0.f));
                auto wt=transform::cached_world(e);
                float tx=finite_val(wt.x), ty=finite_val(wt.y);
                float rot=wt.rotation*(float)M_PI/180.f;
                float c=std::cos(rot), s=std::sin(rot);

                Shape sh; sh.entity=&e; sh.col=&comp; sh.sub_id=(int)out.size();
                sh.material=read_material(comp);

                if (type=="box"){
                    float w = finite_val(col_ref.value("width", 1.f)) * 0.5f;
                    float h = finite_val(col_ref.value("height", 1.f)) * 0.5f;
                    Verts verts;
                    for (auto [dx,dy]:std::initializer_list<Vec2>{{-w,-h},{w,-h},{w,h},{-w,h}})
                        verts.push_back(world_from_local(tx,ty,c,s,ox+dx,oy+dy));
                    sh.kind=ShapeKind::Polygon; sh.verts=ensure_ccw(verts);
                    sh.cx=tx; sh.cy=ty;
                    out.push_back(std::move(sh));
                } else if (type=="circle"){
                    float r = std::max(0.f, finite_val(col_ref.value("radius", 8.f)));
                    auto [cx2,cy2]=world_from_local(tx,ty,c,s,ox,oy);
                    sh.kind=ShapeKind::Circle; sh.cx=cx2; sh.cy=cy2; sh.radius=r;
                    out.push_back(std::move(sh));
                } else if (type=="capsule"){
                    float r = std::max(1e-3f, finite_val(col_ref.value("radius", 8.f)));
                    float h = std::max(0.f, finite_val(col_ref.value("height", 32.f)));
                    bool horiz=col_ref.value("direction","vertical")=="horizontal";
                    sh.kind=ShapeKind::Capsule; sh.radius=r; sh.cap_half_h=std::max(0.f,h*0.5f-r); sh.cap_horiz=horiz;
                    auto [wcx,wcy]=world_from_local(tx,ty,c,s,ox,oy);
                    sh.cx=wcx; sh.cy=wcy;
                    if (horiz){
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox-sh.cap_half_h,oy));
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox+sh.cap_half_h,oy));
                    } else {
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox,oy-sh.cap_half_h));
                        sh.world_pts.push_back(world_from_local(tx,ty,c,s,ox,oy+sh.cap_half_h));
                    }
                    out.push_back(std::move(sh));
                }
            }
        }
    }

    if (!has_component(e,"Tilemap")) return out;
    auto& tm=e["components"]["Tilemap"];
    if (!tm.value("generate_colliders",false)) return out;

    int tile_size=std::max(1,tm.value("tile_size",32));
    int origin_x=tm.value("origin_x",0), origin_y=tm.value("origin_y",0);
    auto& grid=tm["grid"];
    std::unordered_set<int> col_set;
    if (tm.contains("collider_tile_ids"))
        for (auto& id:tm["collider_tile_ids"]) if(id.is_number()) col_set.insert(id.get<int>());
    auto wt=transform::cached_world(e);
    float tx=finite_val(wt.x), ty=finite_val(wt.y);

    int rows=(int)grid.size();
    if (rows==0) return out;
    int cols=(int)grid[0].size();

    auto is_solid=[&](int r, int c)->bool{
        if(r<0||r>=rows||c<0||c>=cols) return false;
        if(grid[r][c].is_null()||!grid[r][c].is_number()) return false;
        int tid=grid[r][c].get<int>();
        if(tid<0) return false;
        if(!col_set.empty()&&!col_set.count(tid)) return false;
        return true;
    };

    std::vector<std::vector<bool>> visited(rows, std::vector<bool>(cols, false));

    auto add_box=[&](int r, int c, int w, int h) {
        float x1 = tx + (c + origin_x) * (float)tile_size;
        float y1 = ty + (r + origin_y) * (float)tile_size;
        float x2 = x1 + w * (float)tile_size;
        float y2 = y1 + h * (float)tile_size;
        Verts verts = {{x1,y1},{x2,y1},{x2,y2},{x1,y2}};
        Shape sh; sh.entity=&e; sh.col=&tm; sh.kind=ShapeKind::Polygon;
        sh.sub_id=(int)out.size(); sh.verts=ensure_ccw(verts);
        sh.cx=(x1+x2)*0.5f; sh.cy=(y1+y2)*0.5f;
        sh.material=PhysicsMaterial();
        out.push_back(sh);
    };

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (visited[r][c] || !is_solid(r, c)) continue;
            int c2 = c;
            while (c2+1<cols && !visited[r][c2+1] && is_solid(r, c2+1)) ++c2;
            int r2 = r;
            for (int rw = r+1; rw < rows; ++rw) {
                bool full = true;
                for (int cx = c; cx <= c2; ++cx) { if (visited[rw][cx]||!is_solid(rw,cx)){full=false;break;} }
                if (full) r2 = rw; else break;
            }
            for (int vr=r;vr<=r2;++vr) for (int vc=c;vc<=c2;++vc) visited[vr][vc]=true;
            add_box(r, c, c2-c+1, r2-r+1);
        }
    }
    return out;
}

static std::optional<Manifold> dispatch_shapes(Shape& s1, Shape& s2) {
    if (s1.kind==ShapeKind::Edge){
        std::optional<Manifold> best;
        bool one_sided = (s1.face_nx != 0.f || s1.face_ny != 0.f);
        for (int i=0;i+1<(int)s1.world_pts.size();++i){
            auto [ax,ay]=s1.world_pts[i]; auto [bx,by]=s1.world_pts[i+1];
            float t=s1.thickness;
            Shape seg; seg.kind=ShapeKind::Polygon; seg.entity=s1.entity; seg.col=s1.col;
            if (one_sided) {
                // One-sided slab: outer face lies exactly on the edge surface,
                // inner face extends t*2 into the tile interior (in face_n direction).
                // This prevents SAT from ever choosing the "push into tile" axis.
                float fnx=s1.face_nx, fny=s1.face_ny; // outward normal
                float inx=-fnx, iny=-fny;               // inward (into tile)
                // Outer edge points (on the surface)
                float ox1=ax, oy1=ay, ox2=bx, oy2=by;
                // Inner edge points (shifted inward by 2*t)
                float ix1=ax+inx*t*2.f, iy1=ay+iny*t*2.f;
                float ix2=bx+inx*t*2.f, iy2=by+iny*t*2.f;
                seg.verts=ensure_ccw({{ox1,oy1},{ox2,oy2},{ix2,iy2},{ix1,iy1}});
            } else {
                // Two-sided fallback (non-tilemap edges like EdgeCollider2D components)
                auto [nx,ny]=normalize(by-ay,ax-bx);
                seg.verts=ensure_ccw({{ax+nx*t,ay+ny*t},{bx+nx*t,by+ny*t},{bx-nx*t,by-ny*t},{ax-nx*t,ay-ny*t}});
            }
            seg.cx=(ax+bx)*0.5f; seg.cy=(ay+by)*0.5f;
            auto m=dispatch_shapes(seg,s2);
            if (m&&m->contact_count>0){
                // For one-sided edges: discard contacts where the manifold normal
                // points inward (into the tile). This is the final guard against
                // SAT resolving in the wrong direction at corner/edge transitions.
                if (one_sided) {
                    float dot = m->nx*s1.face_nx + m->ny*s1.face_ny;
                    if (dot < 0.f) { m->nx=-m->nx; m->ny=-m->ny; }
                }
                if (!best||m->contacts[0].depth>best->contacts[0].depth) best=m;
            }
        }
        return best;
    }
    if (s2.kind==ShapeKind::Edge){
        auto m=dispatch_shapes(s2,s1);
        if (m){m->nx=-m->nx;m->ny=-m->ny;}
        return m;
    }

    // Circle-Circle
    if (s1.kind==ShapeKind::Circle&&s2.kind==ShapeKind::Circle){
        float dx=s2.cx-s1.cx,dy=s2.cy-s1.cy,rr=s1.radius+s2.radius;
        float d2=dx*dx+dy*dy;
        if (d2>=rr*rr) return std::nullopt;
        float d=std::sqrt(d2); if(d<1e-12f){d=1e-9f;dy=1;}
        float nx=dx/d,ny=dy/d;
        Manifold m; m.nx=nx;m.ny=ny;
        m.contacts[0]={s1.cx+nx*s1.radius,s1.cy+ny*s1.radius,rr-d,0,0,0};
        m.contact_count=1;
        return m;
    }
    // Circle-Poly
    if (s1.kind==ShapeKind::Circle&&s2.kind==ShapeKind::Polygon){
        auto m=poly_vs_circle(s2,s1); if(m){m->nx=-m->nx;m->ny=-m->ny;} return m;
    }
    if (s1.kind==ShapeKind::Polygon&&s2.kind==ShapeKind::Circle) return poly_vs_circle(s1,s2);
    // Poly-Poly
    if (s1.kind==ShapeKind::Polygon&&s2.kind==ShapeKind::Polygon) return poly_vs_poly(s1,s2);
    // Capsule combos
    if (s1.kind==ShapeKind::Capsule&&s2.kind==ShapeKind::Capsule) return capsule_vs_capsule(s1,s2);
    if (s1.kind==ShapeKind::Capsule&&s2.kind==ShapeKind::Circle)  return capsule_vs_circle(s1,s2);
    if (s1.kind==ShapeKind::Circle&&s2.kind==ShapeKind::Capsule){ auto m=capsule_vs_circle(s2,s1); if(m){m->nx=-m->nx;m->ny=-m->ny;} return m; }
    if (s1.kind==ShapeKind::Capsule&&s2.kind==ShapeKind::Polygon) return capsule_vs_poly(s1,s2);
    if (s1.kind==ShapeKind::Polygon&&s2.kind==ShapeKind::Capsule){ auto m=capsule_vs_poly(s2,s1); if(m){m->nx=-m->nx;m->ny=-m->ny;} return m; }
    return std::nullopt;
}


// ════════════════════════════════════════════════════════════════════════════
//  ADDITIONAL HELPERS mirrored from physics.cpp (static linkage copies)
// ════════════════════════════════════════════════════════════════════════════
static bool is_static (const Entity* rb) {
    if (!rb||rb->is_null()) return true;
    if (body_type(*rb)=="static") return true;
    return rb->value("mass",1.f)<=0.f;
}

static PhysicsMaterial read_material(const Entity& col) {
    PhysicsMaterial m;
    m.friction   = finite_val(col.value("friction",  0.4f), 0.4f);
    m.bounciness = finite_val(col.value("bounciness",0.0f), 0.0f);
    auto str_mode = [&](const char* key, CombineMode def) -> CombineMode {
        std::string s = col.value(key, std::string{});
        if (s=="minimum")  return CombineMode::Minimum;
        if (s=="maximum")  return CombineMode::Maximum;
        if (s=="multiply") return CombineMode::Multiply;
        return def;
    };
    m.friction_combine = str_mode("friction_combine", CombineMode::Average);
    m.bounce_combine   = str_mode("bounce_combine",   CombineMode::Maximum);

    // Physics material override support
    if (col.contains("material_override")){
        auto& mat_override = col["material_override"];
        m.friction   = finite_val(mat_override.value("friction",  m.friction),  m.friction);
        m.bounciness = finite_val(mat_override.value("bounciness", m.bounciness), m.bounciness);
        m.friction_combine = str_mode("friction_combine", m.friction_combine);
        m.bounce_combine   = str_mode("bounce_combine",   m.bounce_combine);
    }
    return m;
}

static bool poly_is_convex(const Verts& v) {
    int n=(int)v.size();
    if(n<3) return true;
    for(int i=0;i<n;++i){
        Vec2 a=v[i],b=v[(i+1)%n],c=v[(i+2)%n];
        float cross=(b.first-a.first)*(c.second-a.second)-(b.second-a.second)*(c.first-a.first);
        if(cross<-1e-5f) return false; // reflex vertex found
    }
    return true;
}

static void decompose_convex(const Verts& poly, std::vector<Verts>& out, int depth) {
    if(depth>32||poly.size()<3){return;}
    if(poly_is_convex(poly)){out.push_back(poly);return;}
    auto [i,j]=find_best_diagonal(poly);
    if(i<0){out.push_back(poly);return;} // fallback: treat as convex
    int n=(int)poly.size();
    Verts left,right;
    for(int k=i;k!=j;k=(k+1)%n) left.push_back(poly[k]);
    left.push_back(poly[j]);
    for(int k=j;k!=i;k=(k+1)%n) right.push_back(poly[k]);
    right.push_back(poly[i]);
    decompose_convex(ensure_ccw(left), out, depth+1);
    decompose_convex(ensure_ccw(right), out, depth+1);
}

static bool point_in_poly(float px, float py, const Verts& v) {
    bool inside=false; int n=(int)v.size(), j=n-1;
    for (int i=0;i<n;j=i++) {
        auto [xi,yi]=v[i]; auto [xj,yj]=v[j];
        if (((yi>py)!=(yj>py))&&
            (px<(xj-xi)*(py-yi)/((yj-yi)?(yj-yi):1e-12f)+xi))
            inside=!inside;
    }
    return inside;
}

static std::pair<Vec2,float> closest_on_seg(float ax,float ay,float bx,float by,float px,float py){
    float dx=bx-ax, dy=by-ay, l2=dx*dx+dy*dy;
    if (l2<1e-12f) return {{ax,ay},(px-ax)*(px-ax)+(py-ay)*(py-ay)};
    float t=clamp(((px-ax)*dx+(py-ay)*dy)/l2,0.f,1.f);
    Vec2 p={ax+t*dx,ay+t*dy};
    return {p,(px-p.first)*(px-p.first)+(py-p.second)*(py-p.second)};
}

static std::pair<Vec2,float> closest_on_poly(float px,float py,const Verts& v){
    Vec2 best=v[0]; float bd=1e30f;
    for (int i=0;i<(int)v.size();++i){
        auto [p,d2]=closest_on_seg(v[i].first,v[i].second,v[(i+1)%v.size()].first,v[(i+1)%v.size()].second,px,py);
        if (d2<bd){bd=d2;best=p;}
    }
    return {best,bd};
}

static std::optional<Manifold> poly_vs_poly(const Shape& A, const Shape& B) {
    auto ra = sat_one_way(A.verts, B.verts, true);
    if (!ra) return std::nullopt;
    auto rb = sat_one_way(B.verts, A.verts, false);
    if (!rb) return std::nullopt;

    // Choose reference face = least penetration
    bool use_a = (ra->pen <= rb->pen + 0.05f);
    const SATResult& ref = use_a ? *ra : *rb;
    const Verts& ref_v   = use_a ? A.verts : B.verts;
    const Verts& inc_v   = use_a ? B.verts : A.verts;
    float nx = ref.axis.first, ny = ref.axis.second;

    // Ensure normal points from ref → inc
    float inc_cx=0,inc_cy=0;
    for (auto [x,y]:inc_v){inc_cx+=x;inc_cy+=y;}
    inc_cx/=(float)inc_v.size(); inc_cy/=(float)inc_v.size();
    float ref_cx=0,ref_cy=0;
    for (auto [x,y]:ref_v){ref_cx+=x;ref_cy+=y;}
    ref_cx/=(float)ref_v.size(); ref_cy/=(float)ref_v.size();
    if (dot(inc_cx-ref_cx,inc_cy-ref_cy,nx,ny)<0){nx=-nx;ny=-ny;}

    // Reference edge endpoints
    int ei = ref.ref_edge_idx; if(ei<0)ei=0;
    Vec2 rA = ref_v[ei], rB = ref_v[(ei+1)%ref_v.size()];

    // Find incident face on inc: face most anti-parallel to ref normal
    int best_i=0; float best_dot=1e30f;
    int m=(int)inc_v.size();
    for (int i=0;i<m;++i){
        auto [ax,ay]=inc_v[i]; auto [bx,by]=inc_v[(i+1)%m];
        auto [fnx,fny]=normalize(by-ay,ax-bx);
        float d=dot(fnx,fny,nx,ny);
        if (d<best_dot){best_dot=d;best_i=i;}
    }
    Vec2 iA=inc_v[best_i], iB=inc_v[(best_i+1)%m];

    // Clip incident edge to side planes of reference face
    auto [tex,tey]=normalize(rB.first-rA.first, rB.second-rA.second);
    float d0=dot(rA.first,rA.second,tex,tey), d1=dot(rB.first,rB.second,tex,tey);

    Vec2 tmp[2], clip[2];
    tmp[0]=iA; tmp[1]=iB;
    int cnt=clip_segment_to_plane(tmp[0],tmp[1],tex,tey,d0,clip);
    if (cnt<1) return std::nullopt;
    Vec2 tmp2[2];
    std::copy(clip,clip+cnt,tmp2);
    cnt=clip_segment_to_plane(tmp2[0],cnt>1?tmp2[1]:tmp2[0],-tex,-tey,-d1,clip);

    // Keep points behind reference face
    float ref_d = dot(rA.first,rA.second,nx,ny);
    Manifold man; man.nx=nx; man.ny=ny;
    for (int i=0;i<cnt;++i){
        float sep = dot(clip[i].first,clip[i].second,nx,ny) - ref_d;
        if (sep<=0.01f) {
            CP cp; cp.x=clip[i].first; cp.y=clip[i].second;
            cp.depth=std::max(0.f,-sep+ref.pen*0.5f);
            // ── Vertex-pair feature key (gap fix) ────────────────────────────
            // Old formula (best_i*1000+i) collides on polygons with ≥32 verts
            // because best_i can be up to n-1 and i only 0..1, giving overlap.
            // Use a hash that encodes BOTH the incident vertex index (best_i+i)
            // and the reference edge index (ei) so each contact point has a
            // unique stable key across frames regardless of polygon complexity.
            // The Cantor pairing function n*(n+1)/2+k is collision-free for
            // non-negative integers, giving keys up to ~2M for 2000-vert polys.
            {
                int inc_vtx = (best_i + i) % m; // actual incident vertex index
                int ref_vtx = ei;
                int key_a = std::min(inc_vtx, ref_vtx);
                int key_b = std::max(inc_vtx, ref_vtx);
                cp.fkey = key_b * (key_b + 1) / 2 + key_a;  // Cantor pairing
            }
            if (man.contact_count < MAX_CONTACTS)
                man.contacts[man.contact_count++]=cp;
        }
    }
    if (man.contact_count==0) {
        // Fallback: centroid contact
        float cpx=(rA.first+rB.first)*0.5f, cpy=(rA.second+rB.second)*0.5f;
        man.contacts[0]={cpx,cpy,ref.pen,0,0,0};
        man.contact_count=1;
    }
    if (!use_a) { man.nx=-man.nx; man.ny=-man.ny; } // flip back to A→B
    return man;
}

static std::optional<Manifold> poly_vs_circle(const Shape& poly, const Shape& circ) {
    float cx=circ.cx, cy=circ.cy, r=circ.radius;
    const auto& verts=poly.verts;
    if (verts.size()<3) return std::nullopt;

    // SAT axes from polygon edges
    float best=1e30f; Vec2 baxis={0,1};
    int n=(int)verts.size();
    for (int i=0;i<n;++i){
        auto [ax,ay]=verts[i]; auto [bx,by]=verts[(i+1)%n];
        auto [nx,ny]=normalize(by-ay,ax-bx);
        float mn=1e30f,mx=-1e30f;
        for (auto [px,py]:verts){float d=dot(px,py,nx,ny);mn=std::min(mn,d);mx=std::max(mx,d);}
        float cproj=dot(cx,cy,nx,ny);
        float ov=std::min(mx,cproj+r)-std::max(mn,cproj-r);
        if (ov<=0) return std::nullopt;
        if (ov<best){best=ov;baxis={nx,ny};}
    }
    // Axis from poly boundary to circle center
    auto [closest,d2]=closest_on_poly(cx,cy,verts);
    if (d2>1e-12f){
        float dx=cx-closest.first, dy=cy-closest.second;
        auto [nx2,ny2]=normalize(dx,dy);
        float mn=1e30f,mx=-1e30f;
        for(auto [px,py]:verts){float d=dot(px,py,nx2,ny2);mn=std::min(mn,d);mx=std::max(mx,d);}
        float cproj=dot(cx,cy,nx2,ny2);
        float ov=std::min(mx,cproj+r)-std::max(mn,cproj-r);
        if(ov>0&&ov<best){best=ov;baxis={nx2,ny2};}
    }
    // Orient normal from poly center toward circle
    float pcx=0,pcy=0;
    for(auto [x,y]:verts){pcx+=x;pcy+=y;}
    pcx/=n; pcy/=n;
    auto [nx,ny]=baxis;
    if(dot(cx-pcx,cy-pcy,nx,ny)<0){nx=-nx;ny=-ny;}

    // Contact point: closest point on poly boundary (or circle surface)
    Manifold m; m.nx=nx; m.ny=ny;
    m.contacts[0]={closest.first,closest.second,best,0,0,0};
    m.contact_count=1;
    return m;
}

static std::optional<Manifold> capsule_vs_circle(const Shape& cap, const Shape& circ) {
    Vec2 A=cap.world_pts[0], B=cap.world_pts[1];
    float r1=cap.radius, r2=circ.radius;
    auto [closest,d2]=closest_on_seg(A.first,A.second,B.first,B.second,circ.cx,circ.cy);
    float rr=r1+r2;
    if (d2>rr*rr) return std::nullopt;
    float d=std::sqrt(d2);
    float nx2,ny2;
    if (d<1e-12f){ nx2=0; ny2=-1; }
    else {
        auto [n1,n2]=normalize(circ.cx-closest.first,circ.cy-closest.second);
        nx2=n1; ny2=n2;
    }
    Manifold m; m.nx=nx2; m.ny=ny2;
    m.contacts[0]={closest.first+nx2*r1,closest.second+ny2*r1,rr-d,0,0,0};
    m.contact_count=1;
    return m;
}

static std::optional<Manifold> capsule_vs_capsule(const Shape& A, const Shape& B) {
    // Segment-segment closest points
    Vec2 p1=A.world_pts[0],p2=A.world_pts[1],p3=B.world_pts[0],p4=B.world_pts[1];
    float d1x=p2.first-p1.first,d1y=p2.second-p1.second;
    float d2x=p4.first-p3.first,d2y=p4.second-p3.second;
    float r1x=p1.first-p3.first,r1y=p1.second-p3.second;
    float a=len2(d1x,d1y), e=len2(d2x,d2y);
    float f=dot(d2x,d2y,r1x,r1y);
    float s,t;
    if (a<1e-12f&&e<1e-12f){ s=t=0; }
    else if (a<1e-12f){ s=0; t=clamp(f/e,0.f,1.f); }
    else {
        float c=dot(d1x,d1y,r1x,r1y);
        if (e<1e-12f){ t=0; s=clamp(-c/a,0.f,1.f); }
        else {
            float b=dot(d1x,d1y,d2x,d2y), denom=a*e-b*b;
            if (std::abs(denom)>1e-12f) s=clamp((b*f-c*e)/denom,0.f,1.f); else s=0;
            t=clamp((b*s+f)/e,0.f,1.f);
            s=clamp((b*t-c)/a,0.f,1.f);
            t=clamp((b*s+f)/e,0.f,1.f);
        }
    }
    Vec2 cA={p1.first+d1x*s,p1.second+d1y*s};
    Vec2 cB={p3.first+d2x*t,p3.second+d2y*t};
    float rr=A.radius+B.radius;
    float dx=cB.first-cA.first, dy=cB.second-cA.second;
    float d2=dx*dx+dy*dy;
    if (d2>rr*rr) return std::nullopt;
    float d=std::sqrt(d2);
    float nx,ny;
    if (d<1e-12f){nx=0;ny=-1;}
    else{nx=dx/d;ny=dy/d;}
    Manifold m; m.nx=nx; m.ny=ny;
    float depth=rr-d;
    m.contacts[0]={cA.first+nx*A.radius,cA.second+ny*A.radius,depth,0,0,0};
    m.contact_count=1;
    return m;
}

static std::optional<Manifold> capsule_vs_poly(const Shape& cap, const Shape& poly) {
    // Test the capsule as a "swept circle": find closest point on polygon boundary
    // to the capsule's medial segment, then check against radius.
    Vec2 A = cap.world_pts[0], B = cap.world_pts[1];
    float r = cap.radius;
    const auto& verts = poly.verts;
    int n = (int)verts.size();
    if (n < 3) return std::nullopt;

    float best_depth = -1e30f;
    Vec2  best_norm  = {0, -1};
    Vec2  best_cp    = {cap.cx, cap.cy};

    // ① Test each polygon edge against the capsule medial segment
    for (int i = 0; i < n; ++i) {
        Vec2 ea = verts[i], eb = verts[(i+1)%n];
        // Edge normal (outward for CCW poly)
        float enx = eb.second - ea.second, eny = -(eb.first - ea.first);
        float elen = std::hypot(enx, eny);
        if (elen < 1e-12f) continue;
        enx /= elen; eny /= elen;

        // Closest point on edge to each capsule endpoint
        for (int k = 0; k < 2; ++k) {
            Vec2 P = (k == 0) ? A : B;
            auto [cp, d2] = closest_on_seg(ea.first, ea.second, eb.first, eb.second, P.first, P.second);
            float d = std::sqrt(d2);
            float depth = r - d;
            if (depth > best_depth) {
                best_depth = depth;
                // Normal from poly edge toward capsule endpoint
                if (d < 1e-12f) { best_norm = {enx, eny}; }
                else { best_norm = {(P.first - cp.first)/d, (P.second - cp.second)/d}; }
                best_cp = {cp.first + best_norm.first * (r - depth),
                           cp.second + best_norm.second * (r - depth)};
            }
        }

        // Also: closest point on medial segment to the edge midpoint
        Vec2 em = {(ea.first+eb.first)*0.5f, (ea.second+eb.second)*0.5f};
        auto [seg_cp, seg_d2] = closest_on_seg(A.first, A.second, B.first, B.second, em.first, em.second);
        float seg_d = std::sqrt(seg_d2);
        float depth2 = r - seg_d;
        if (depth2 > best_depth) {
            best_depth = depth2;
            if (seg_d < 1e-12f) { best_norm = {enx, eny}; }
            else { best_norm = {(em.first - seg_cp.first)/seg_d, (em.second - seg_cp.second)/seg_d}; }
            best_cp = {seg_cp.first + best_norm.first * r, seg_cp.second + best_norm.second * r};
        }
    }

    // ② Check if center of caps is inside polygon (full containment)
    if (point_in_poly(cap.cx, cap.cy, verts)) {
        // Use SAT axes to find shallowest penetration axis for extraction
        Shape cap_circ; cap_circ.kind=ShapeKind::Circle; cap_circ.entity=cap.entity;
        cap_circ.col=cap.col; cap_circ.cx=cap.cx; cap_circ.cy=cap.cy; cap_circ.radius=cap.radius;
        auto m = poly_vs_circle(poly, cap_circ);
        if (m) return m;
    }

    if (best_depth <= 0) return std::nullopt;

    Manifold man;
    man.nx = best_norm.first; man.ny = best_norm.second;
    man.contacts[0] = {best_cp.first, best_cp.second, best_depth, 0, 0, 0, 0};
    man.contact_count = 1;

    // ③ Try to generate a second contact point when the capsule's flat side
    //    is nearly parallel to a polygon edge (common for standing on a flat surface).
    //    We test both capsule endpoints against the best-normal direction and add the
    //    second one if it's also penetrating. This gives 2-point manifolds like Box2D.
    if (man.contact_count < MAX_CONTACTS) {
        // The "other" endpoint relative to best_cp
        Vec2 other_pt = (cap.world_pts[0].first != best_cp.first ||
                         cap.world_pts[0].second != best_cp.second - cap.radius * best_norm.second)
                        ? A : B;
        // Project other endpoint onto the contact normal and check penetration
        float ref_d = dot(best_cp.first - best_norm.first * cap.radius,
                          best_cp.second - best_norm.second * cap.radius,
                          best_norm.first, best_norm.second);
        // Closest poly point to the other cap endpoint
        auto [cp2, d2_2] = closest_on_poly(other_pt.first, other_pt.second, verts);
        float depth2 = cap.radius - std::sqrt(d2_2);
        if (depth2 > 0.f && depth2 > best_depth * 0.3f) {
            // Check that normal is similar (same face)
            Vec2 n2 = {0.f, 0.f};
            if (d2_2 > 1e-12f) {
                float d2s = std::sqrt(d2_2);
                n2 = {(other_pt.first - cp2.first)/d2s, (other_pt.second - cp2.second)/d2s};
            }
            float ndot = dot(n2.first, n2.second, best_norm.first, best_norm.second);
            if (ndot > 0.5f) {  // normals agree — same contact face
                CP cp2_cp;
                cp2_cp.x = cp2.first + best_norm.first * (cap.radius - depth2);
                cp2_cp.y = cp2.second + best_norm.second * (cap.radius - depth2);
                cp2_cp.depth = depth2;
                cp2_cp.fkey  = 1;
                man.contacts[man.contact_count++] = cp2_cp;
            }
        }
    }

    return man;
}

static float body_inv_mass(const Entity* rb){
    if (!rb||body_type(*rb)!="dynamic") return 0.f;
    return 1.f/std::max(finite_val(rb->value("mass",1.f),1.f),1e-9f);
}

// enable_interpolation is defined in physics.cpp; forward-declare to resolve calls from this TU
void enable_interpolation(Entity& entity, bool enable);


// ════════════════════════════════════════════════════════════════════════════
//  ①  GLOBAL GRAVITY DIRECTION
// ════════════════════════════════════════════════════════════════════════════
void set_gravity_direction(float gx, float gy) {
    float d = std::hypot(gx, gy);
    if (d < 1e-12f) { s_gravity_dir = {0.f, 1.f}; return; }
    s_gravity_dir = {gx/d, gy/d};
}

Vec2 get_gravity_direction() { return s_gravity_dir; }

void set_gravity_vector(float gx, float gy) {
    s_gravity_mag = std::hypot(gx, gy);
    set_gravity_direction(gx, gy);
    // Also push the full gravity vector into the core integrator so that
    // apply_physics() (not just apply_physics_ext) respects the direction.
    phys_set_gravity_override(gx, gy, true);
}

void reset_gravity_vector() {
    // Revert to default downward gravity; core integrator uses gravity param again.
    phys_set_gravity_override(0.f, 0.f, false);
    s_gravity_dir = {0.f, 1.f};
}

Vec2 get_gravity_vector() {
    return {s_gravity_dir.first * s_gravity_mag,
            s_gravity_dir.second * s_gravity_mag};
}

// ════════════════════════════════════════════════════════════════════════════
//  ②  BUOYANCY EFFECTOR 2D
// ════════════════════════════════════════════════════════════════════════════
// ─── Shape-aware partial submersion (Phase 3) ────────────────────────────────
// The previous implementation used (surface_y - aabb_top) / aabb_height as a
// stand-in for submerged fraction — correct for an axis-aligned box, but
// wrong for anything else: a circle submerged 20% by height has noticeably
// less than 20% of its area underwater (and a rotated polygon's true
// submerged area can differ a lot from its AABB's). This computes the
// actual submerged area for circles and polygons by clipping against the
// horizontal surface line, returning {submerged_area, total_area,
// submerged_centroid_x, submerged_centroid_y}. Capsule/Edge shapes fall back
// to polygon clipping using their already-computed world_pts/verts where
// possible, else to the AABB proxy (kept only as a last-resort fallback).
struct SubmersionResult { float submerged_area=0.f, total_area=1.f, cx=0.f, cy=0.f; bool valid=false; };

static SubmersionResult submerged_area_circle(float ccx, float ccy, float r, float surface_y) {
    SubmersionResult res;
    res.total_area = (float)M_PI * r * r;
    if (surface_y <= ccy - r) { res.cx=ccx; res.cy=ccy-r*0.5f; res.valid=true; return res; } // fully above
    if (surface_y >= ccy + r) { res.submerged_area=res.total_area; res.cx=ccx; res.cy=ccy; res.valid=true; return res; } // fully below

    // Circular segment below the line y = surface_y (y grows downward).
    // h = depth of the submerged cap measured from the bottom of the circle.
    float h = surface_y - (ccy - r); // 0..2r
    h = clamp(h, 0.f, 2.f*r);
    // Standard circular segment area formula using the half-angle at center.
    float d = r - h;                       // distance from center to chord (signed, d<0 if cap > half circle)
    float theta = 2.f * std::acos(clamp(d / std::max(r,1e-9f), -1.f, 1.f)); // full angle subtended
    float seg_area = 0.5f * r * r * (theta - std::sin(theta));
    res.submerged_area = clamp(seg_area, 0.f, res.total_area);
    // Centroid of a circular segment lies on the symmetry axis (here vertical,
    // since the cut is horizontal) at distance (4r sin³(θ/2))/(3(θ - sinθ)) from center.
    float denom = 3.f*(theta - std::sin(theta));
    float dist_from_center = (std::abs(denom) > 1e-6f)
        ? (4.f*r*std::pow(std::sin(theta*0.5f),3.f)) / denom
        : 0.f;
    res.cx = ccx;
    res.cy = ccy + dist_from_center; // segment is the lower cap, so centroid is below center
    res.valid = true;
    return res;
}

// Sutherland-Hodgman clip of a CCW polygon against the half-plane y <= surface_y
// (keeps the submerged part, since y grows downward in this engine's convention).
static Verts clip_polygon_below_line(const Verts& poly, float surface_y) {
    Verts out;
    int n=(int)poly.size();
    if (n<3) return out;
    for (int i=0;i<n;++i) {
        auto [x1,y1]=poly[i];
        auto [x2,y2]=poly[(i+1)%n];
        bool in1 = y1 <= surface_y;
        bool in2 = y2 <= surface_y;
        if (in1) out.push_back({x1,y1});
        if (in1 != in2) {
            // Edge crosses the surface line — interpolate intersection point.
            float t = (surface_y - y1) / ((y2 - y1) != 0.f ? (y2 - y1) : 1e-9f);
            out.push_back({x1 + (x2-x1)*t, surface_y});
        }
    }
    return out;
}

static SubmersionResult submerged_area_polygon(const Verts& world_verts, float surface_y) {
    SubmersionResult res;
    res.total_area = std::abs(poly_area(world_verts));
    if (res.total_area < 1e-9f) return res;
    Verts clipped = clip_polygon_below_line(world_verts, surface_y);
    if (clipped.size() < 3) { res.valid=true; return res; } // nothing submerged
    auto [cx,cy,area,_iz] = poly_mass_data(clipped);
    res.submerged_area = clamp(area, 0.f, res.total_area);
    res.cx = cx; res.cy = cy;
    res.valid = true;
    return res;
}

// Dispatches on shape kind to compute true submersion for one Shape.
static SubmersionResult compute_submersion(const Shape& sh, float surface_y) {
    if (sh.kind == ShapeKind::Circle) {
        return submerged_area_circle(sh.cx, sh.cy, sh.radius, surface_y);
    }
    if (sh.kind == ShapeKind::Polygon && sh.verts.size() >= 3) {
        return submerged_area_polygon(sh.verts, surface_y);
    }
    if (sh.kind == ShapeKind::Capsule) {
        // Approximate capsule submersion as: rectangle body (between the two
        // cap centers, full width = 2r) clipped by the line, plus two circle
        // caps clipped by the same line. This is exact for an axis-aligned
        // capsule and a good approximation for rotated ones.
        if (sh.world_pts.size() < 2) { SubmersionResult r; r.valid=false; return r; }
        auto [p1x,p1y] = sh.world_pts[0];
        auto [p2x,p2y] = sh.world_pts[1];
        float dx=p2x-p1x, dy=p2y-p1y;
        float len = std::hypot(dx,dy);
        float nx = len>1e-9f ? -dy/len : 0.f, ny = len>1e-9f ? dx/len : 1.f; // perpendicular
        Verts rect = ensure_ccw({
            {p1x+nx*sh.radius,p1y+ny*sh.radius}, {p2x+nx*sh.radius,p2y+ny*sh.radius},
            {p2x-nx*sh.radius,p2y-ny*sh.radius}, {p1x-nx*sh.radius,p1y-ny*sh.radius}
        });
        SubmersionResult rect_res = submerged_area_polygon(rect, surface_y);
        SubmersionResult cap1 = submerged_area_circle(p1x,p1y,sh.radius,surface_y);
        SubmersionResult cap2 = submerged_area_circle(p2x,p2y,sh.radius,surface_y);
        SubmersionResult res;
        // Rectangle area already double-counts half of each circular cap
        // (the caps extend the rect's flat ends) — close enough for a 2D
        // buoyancy proxy without going to full Minkowski-sum exactness.
        res.total_area = rect_res.total_area + (float)M_PI*sh.radius*sh.radius;
        res.submerged_area = clamp(rect_res.submerged_area + cap1.submerged_area*0.5f + cap2.submerged_area*0.5f, 0.f, res.total_area);
        float wsum = rect_res.submerged_area + cap1.submerged_area*0.5f + cap2.submerged_area*0.5f;
        if (wsum > 1e-6f) {
            res.cx = (rect_res.cx*rect_res.submerged_area + cap1.cx*cap1.submerged_area*0.5f + cap2.cx*cap2.submerged_area*0.5f) / wsum;
            res.cy = (rect_res.cy*rect_res.submerged_area + cap1.cy*cap1.submerged_area*0.5f + cap2.cy*cap2.submerged_area*0.5f) / wsum;
        } else { res.cx=sh.cx; res.cy=sh.cy; }
        res.valid = true;
        return res;
    }
    SubmersionResult r; r.valid=false; return r; // Edge or unhandled — caller falls back to AABB proxy
}

void apply_buoyancy_effectors(EntityList& entities, float global_gravity) {
    // Collect all BuoyancyEffector2D zones
    struct BuoyZone {
        float surface_level, density, lin_drag, ang_drag, flow_angle, flow_mag;
        // AABB of the collider that hosts the effector (used for containment test)
        float x1,y1,x2,y2;
        Entity* host;
    };
    std::vector<BuoyZone> zones;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e, "BuoyancyEffector2D")) continue;
        auto& be = e["components"]["BuoyancyEffector2D"];
        // Get AABB from host collider
        auto shapes = collect_shapes(e); // reuse existing internal helper
        if (shapes.empty()) continue;
        auto [bx1,by1,bx2,by2] = shape_aabb(shapes[0]);
        BuoyZone z;
        z.host         = &e;
        z.surface_level= finite_val(be.value("surface_level", by1));
        z.density      = finite_val(be.value("density",        1.f),  1.f);
        z.lin_drag     = finite_val(be.value("linear_drag",    1.f),  0.f);
        z.ang_drag     = finite_val(be.value("angular_drag",   1.f),  0.f);
        z.flow_angle   = finite_val(be.value("flow_angle",     0.f));
        z.flow_mag     = finite_val(be.value("flow_magnitude", 0.f));
        z.x1=bx1; z.y1=by1; z.x2=bx2; z.y2=by2;
        zones.push_back(z);
    }
    if (zones.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb  = e["components"]["Rigidbody2D"];
        auto& tr  = e["components"]["Transform"];
        if (body_type(rb) != "dynamic") continue;
        if (rb.value("_sleeping", false)) continue;

        float mass = std::max(finite_val(rb.value("mass",1.f),1.f), 1e-9f);

        auto shapes = collect_shapes(e);
        if (shapes.empty()) continue;
        auto [sx1,sy1,sx2,sy2] = shape_aabb(shapes[0]);
        float body_h = sy2 - sy1;
        float aabb_area = (sx2-sx1) * body_h;

        float cx = (sx1+sx2)*0.5f;
        float cy = (sy1+sy2)*0.5f;

        for (auto& z : zones) {
            // ── Effector layer mask enforcement (gap fix) ─────────────────────
            // _effector_layer_mask was stored but never checked. Check it here
            // so BuoyancyEffector2D correctly skips bodies on non-matching layers.
            if (!effector_affects_layer(*z.host, "BuoyancyEffector2D",
                                        rb.value("layer", 0))) continue;
            // Quick AABB rejection (body's centroid must be inside zone XY)
            if (cx < z.x1 || cx > z.x2 || cy < z.y1 || cy > z.y2) continue;
            if (z.surface_level <= sy1) continue; // entirely above the surface

            // True shape-aware submerged area (circle segment / polygon clip),
            // falling back to the AABB height-fraction proxy only for shapes
            // we don't have exact clipping for (e.g. Edge).
            SubmersionResult sub = compute_submersion(shapes[0], z.surface_level);
            float body_area, submerged_frac;
            if (sub.valid) {
                body_area = std::max(sub.total_area, 1e-6f);
                submerged_frac = clamp(sub.submerged_area / body_area, 0.f, 1.f);
            } else {
                body_area = std::max(aabb_area, 1e-6f);
                submerged_frac = (z.surface_level >= sy2) ? 1.f
                    : clamp((z.surface_level - sy1) / std::max(body_h, 1e-6f), 0.f, 1.f);
            }
            if (submerged_frac < 1e-4f) continue;

            // Archimedes upward force: F = density * g * submerged_volume
            // (We use area as a proxy for 2D "volume"; density is fluid density)
            float buoy_force = z.density * global_gravity * body_area * submerged_frac;
            // Gravity direction is s_gravity_dir; buoyancy opposes it
            float bfx = -s_gravity_dir.first  * buoy_force;
            float bfy = -s_gravity_dir.second * buoy_force;
            rb["_force_x"] = finite_val(rb.value("_force_x",0.f)) + bfx;
            rb["_force_y"] = finite_val(rb.value("_force_y",0.f)) + bfy;
            // Buoyancy applied at the submerged centroid (not body center)
            // produces a righting torque for partially-submerged asymmetric
            // shapes — e.g. a tilted box floats back level instead of just
            // translating, matching Unity's per-point buoyancy sampling.
            if (sub.valid && !rb.value("freeze_rotation", false)) {
                float rx = sub.cx - cx, ry = sub.cy - cy;
                float torque = cross(rx, ry, bfx, bfy);
                rb["_torque"] = finite_val(rb.value("_torque",0.f)) + torque;
            }

            // Linear drag inside fluid
            float vx = finite_val(rb.value("velocity_x",0.f));
            float vy = finite_val(rb.value("velocity_y",0.f));
            float drag_mult = std::max(0.f, 1.f - z.lin_drag * submerged_frac * (1.f/60.f));
            rb["velocity_x"] = vx * drag_mult;
            rb["velocity_y"] = vy * drag_mult;

            // Angular drag inside fluid
            float av = finite_val(rb.value("angular_velocity",0.f));
            float adrag_mult = std::max(0.f, 1.f - z.ang_drag * submerged_frac * (1.f/60.f));
            rb["angular_velocity"] = av * adrag_mult;

            // Current / flow force
            if (z.flow_mag > 1e-6f) {
                float fa = z.flow_angle * (float)M_PI / 180.f;
                float flow_fx = std::cos(fa) * z.flow_mag * z.density * submerged_frac;
                float flow_fy = std::sin(fa) * z.flow_mag * z.density * submerged_frac;
                rb["_force_x"] = finite_val(rb.value("_force_x",0.f)) + flow_fx;
                rb["_force_y"] = finite_val(rb.value("_force_y",0.f)) + flow_fy;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ③  CONSTANT FORCE 2D
// ════════════════════════════════════════════════════════════════════════════
void apply_constant_force2d(EntityList& entities) {
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"ConstantForce2D")) continue;
        if (!has_component(e,"Rigidbody2D"))     continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        auto& cf = e["components"]["ConstantForce2D"];

        float fx = finite_val(cf.value("force_x",0.f));
        float fy = finite_val(cf.value("force_y",0.f));
        float torque = finite_val(cf.value("torque",0.f));

        // Relative force: rotate by body angle
        float rfx = finite_val(cf.value("relative_force_x",0.f));
        float rfy = finite_val(cf.value("relative_force_y",0.f));
        if (std::hypot(rfx,rfy) > 1e-9f && has_component(e,"Transform")) {
            float rot = finite_val(get_float(e["components"]["Transform"],"rotation"))
                        * (float)M_PI / 180.f;
            float cr = std::cos(rot), sr = std::sin(rot);
            fx += rfx*cr - rfy*sr;
            fy += rfx*sr + rfy*cr;
        }

        rb["_force_x"]  = finite_val(rb.value("_force_x",0.f))  + fx;
        rb["_force_y"]  = finite_val(rb.value("_force_y",0.f))  + fy;
        rb["_torque"]   = finite_val(rb.value("_torque",0.f))   + torque;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ④  POINT EFFECTOR 2D (full implementation)
// ════════════════════════════════════════════════════════════════════════════
void apply_point_effectors2(EntityList& entities) {
    struct PtEffector {
        float cx, cy;                 // world origin
        float force_magnitude;
        float distance_scale;
        float angular_drag, linear_drag;
        std::string force_mode;       // "constant" | "inverse_linear" | "inverse_square"
        int   layer_mask;
        bool  use_collider_mask;
        Entity* host;
    };
    std::vector<PtEffector> effectors;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"PointEffector2D")) continue;
        if (!has_component(e,"Transform")) continue;
        auto& pe = e["components"]["PointEffector2D"];
        auto& tr = e["components"]["Transform"];
        PtEffector p;
        p.host            = &e;
        p.cx              = finite_val(get_float(tr,"x"));
        p.cy              = finite_val(get_float(tr,"y"));
        p.force_magnitude = finite_val(pe.value("force_magnitude", 10.f));
        p.distance_scale  = std::max(1e-6f, finite_val(pe.value("distance_scale",1.f),1.f));
        p.angular_drag    = finite_val(pe.value("angular_drag",   0.f));
        p.linear_drag     = finite_val(pe.value("linear_drag",    0.f));
        p.force_mode      = pe.value("force_mode", std::string("inverse_linear"));
        p.layer_mask      = pe.value("layer_mask", 0xFFFF);
        p.use_collider_mask = pe.value("use_collider_mask", true);
        effectors.push_back(p);
    }
    if (effectors.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        if (rb.value("_sleeping",false)) continue;

        float bx = finite_val(get_float(e["components"]["Transform"],"x"));
        float by = finite_val(get_float(e["components"]["Transform"],"y"));
        int   layer = rb.value("layer",0);

        for (auto& p : effectors) {
            if (p.host == &e) continue;
            if (p.use_collider_mask && !(p.layer_mask & (1<<layer))) continue;
            float dx = bx - p.cx, dy = by - p.cy;
            float dist = std::hypot(dx,dy);
            if (dist < 1e-6f) continue;

            float scaled_dist = dist / p.distance_scale;
            float force_mag = 0.f;
            if (p.force_mode == "constant") {
                force_mag = p.force_magnitude;
            } else if (p.force_mode == "inverse_square") {
                force_mag = p.force_magnitude / std::max(scaled_dist*scaled_dist, 1e-6f);
            } else { // inverse_linear (default)
                force_mag = p.force_magnitude / std::max(scaled_dist, 1e-6f);
            }

            // Positive magnitude = attract (toward effector), negative = repel
            float nx = -dx/dist, ny = -dy/dist; // toward effector
            rb["_force_x"] = finite_val(rb.value("_force_x",0.f)) + nx*force_mag;
            rb["_force_y"] = finite_val(rb.value("_force_y",0.f)) + ny*force_mag;

            // Drag
            if (p.linear_drag > 1e-9f) {
                float vx = finite_val(rb.value("velocity_x",0.f));
                float vy = finite_val(rb.value("velocity_y",0.f));
                rb["velocity_x"] = vx * std::max(0.f, 1.f - p.linear_drag*(1.f/60.f));
                rb["velocity_y"] = vy * std::max(0.f, 1.f - p.linear_drag*(1.f/60.f));
            }
            if (p.angular_drag > 1e-9f) {
                float av = finite_val(rb.value("angular_velocity",0.f));
                rb["angular_velocity"] = av * std::max(0.f, 1.f - p.angular_drag*(1.f/60.f));
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑤  AREA EFFECTOR 2D — full implementation
// ════════════════════════════════════════════════════════════════════════════
void apply_area_effectors2(EntityList& entities) {
    // Build list of area effectors with their polygon shapes for containment test
    struct AreaEff {
        float force_angle, force_magnitude, force_variation;
        float drag, angular_drag;
        bool  use_global_angle;
        float host_rotation;   // in degrees, for relative angle mode
        // Pre-baked world-space force vector (updated below)
        float fx, fy;
        // AABB for fast rejection
        float x1,y1,x2,y2;
        // Exact polygon for containment (from BoxCollider2D / PolygonCollider2D)
        Verts poly;
        Entity* host;
    };

    std::vector<AreaEff> effectors;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"AreaEffector2D")) continue;
        auto& ae = e["components"]["AreaEffector2D"];
        AreaEff a;
        a.host             = &e;
        a.force_angle      = finite_val(ae.value("force_angle",     0.f));
        a.force_magnitude  = finite_val(ae.value("force_magnitude", 10.f));
        a.force_variation  = finite_val(ae.value("force_variation",  0.f));
        a.drag             = finite_val(ae.value("drag",            0.f));
        a.angular_drag     = finite_val(ae.value("angular_drag",    0.f));
        a.use_global_angle = ae.value("use_global_angle", true);
        a.host_rotation    = has_component(e,"Transform")
                             ? finite_val(get_float(e["components"]["Transform"],"rotation"))
                             : 0.f;

        // Collect shape for containment
        auto shapes = collect_shapes(e);
        if (shapes.empty()) continue;
        auto [bx1,by1,bx2,by2] = shape_aabb(shapes[0]);
        a.x1=bx1; a.y1=by1; a.x2=bx2; a.y2=by2;
        if (shapes[0].kind == ShapeKind::Polygon) {
            a.poly = shapes[0].verts;
        }

        // Pre-compute force vector (re-rolled each call for variation)
        float var = 0.f;
        if (a.force_variation > 1e-6f) {
            std::uniform_real_distribution<float> dist(-a.force_variation, a.force_variation);
            var = dist(s_rng);
        }
        float effective_angle = a.force_angle;
        if (!a.use_global_angle) effective_angle += a.host_rotation;
        float fa = effective_angle * (float)M_PI / 180.f;
        float mag = a.force_magnitude + var;
        a.fx = std::cos(fa) * mag;
        a.fy = std::sin(fa) * mag;
        effectors.push_back(a);
    }
    if (effectors.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        if (rb.value("_sleeping",false)) continue;

        float bx = finite_val(get_float(e["components"]["Transform"],"x"));
        float by = finite_val(get_float(e["components"]["Transform"],"y"));

        for (auto& a : effectors) {
            if (a.host == &e) continue;
            // AABB quick reject
            if (bx < a.x1 || bx > a.x2 || by < a.y1 || by > a.y2) continue;
            // Exact containment
            if (!a.poly.empty() && !point_in_poly(bx, by, a.poly)) continue;

            // Apply force
            rb["_force_x"] = finite_val(rb.value("_force_x",0.f)) + a.fx;
            rb["_force_y"] = finite_val(rb.value("_force_y",0.f)) + a.fy;

            // Apply drag overrides
            if (a.drag > 1e-9f) {
                float vx = finite_val(rb.value("velocity_x",0.f));
                float vy = finite_val(rb.value("velocity_y",0.f));
                rb["velocity_x"] = vx * std::max(0.f, 1.f - a.drag*(1.f/60.f));
                rb["velocity_y"] = vy * std::max(0.f, 1.f - a.drag*(1.f/60.f));
            }
            if (a.angular_drag > 1e-9f) {
                float av = finite_val(rb.value("angular_velocity",0.f));
                rb["angular_velocity"] = av * std::max(0.f, 1.f - a.angular_drag*(1.f/60.f));
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑥  PULLEY JOINT 2D
// ════════════════════════════════════════════════════════════════════════════
void solve_pulley_joints(EntityList& entities, float dt,
                         std::unordered_map<int,Entity*>& emap)
{
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"PulleyJoint2D")) continue;
        if (!has_component(e,"Rigidbody2D")||!has_component(e,"Transform")) continue;
        auto& pj  = e["components"]["PulleyJoint2D"];
        auto& rb1 = e["components"]["Rigidbody2D"];
        auto& t1  = e["components"]["Transform"];
        if (is_static(&rb1)) continue;

        int oid = pj.value("connected_entity",-1);
        if (!emap.count(oid)) continue;
        auto* other = emap[oid];
        if (!has_component(*other,"Rigidbody2D")||!has_component(*other,"Transform")) continue;
        auto& rb2 = (*other)["components"]["Rigidbody2D"];
        auto& t2  = (*other)["components"]["Transform"];

        float ax = finite_val(pj.value("ground_anchor_ax", get_float(t1,"x")));
        float ay = finite_val(pj.value("ground_anchor_ay", get_float(t1,"y") - 100.f));
        float bx = finite_val(pj.value("ground_anchor_bx", get_float(t2,"x")));
        float by = finite_val(pj.value("ground_anchor_by", get_float(t2,"y") - 100.f));
        float ratio = std::max(1e-6f, finite_val(pj.value("ratio",1.f),1.f));

        // Body-A anchor offset (local to body, stored as world for simplicity)
        float a1x = finite_val(get_float(t1,"x"));
        float a1y = finite_val(get_float(t1,"y"));
        float a2x = finite_val(get_float(t2,"x"));
        float a2y = finite_val(get_float(t2,"y"));

        // Direction vectors from bodies to ground anchors
        float d1x=ax-a1x, d1y=ay-a1y;
        float d2x=bx-a2x, d2y=by-a2y;
        float len1 = std::hypot(d1x,d1y); if(len1<1e-6f)len1=1e-6f;
        float len2 = std::hypot(d2x,d2y); if(len2<1e-6f)len2=1e-6f;
        float n1x=d1x/len1, n1y=d1y/len1;
        float n2x=d2x/len2, n2y=d2y/len2;

        // Cache total length on first use
        if (!pj.contains("_total_length")) {
            pj["_total_length"] = len1 + ratio*len2;
        }
        float total = finite_val(pj.value("_total_length", len1+ratio*len2));

        // Constraint: C = len1 + ratio*len2 - total = 0
        float C = len1 + ratio*len2 - total;

        // Constraint Jacobian: J*v = -C_dot
        // J = [-n1, 0,  -ratio*n2, 0]  (translational only, no torque contribution)
        float im1 = body_inv_mass(&rb1);
        float im2 = body_inv_mass(&rb2);
        float K = im1 + ratio*ratio*im2;
        if (K < 1e-12f) continue;

        float vx1=finite_val(rb1.value("velocity_x",0.f)), vy1=finite_val(rb1.value("velocity_y",0.f));
        float vx2=finite_val(rb2.value("velocity_x",0.f)), vy2=finite_val(rb2.value("velocity_y",0.f));

        // C_dot = n1•v1 + ratio*n2•v2  (projected velocities along rope directions)
        float Cdot = dot(n1x,n1y,vx1,vy1) + ratio*dot(n2x,n2y,vx2,vy2);
        // Baumgarte bias
        float bias = BAUMGARTE * C / dt;
        float lambda = -(Cdot + bias) / K;

        // Accumulate (no clamping for pulley — it can push or pull)
        float old_lambda = finite_val(pj.value("_lambda",0.f));
        float new_lambda = old_lambda + lambda;
        pj["_lambda"] = new_lambda;
        float dl = new_lambda - old_lambda;

        // Apply impulses
        rb1["velocity_x"] = vx1 + n1x*dl*im1;
        rb1["velocity_y"] = vy1 + n1y*dl*im1;
        if (!is_static(&rb2)) {
            rb2["velocity_x"] = vx2 + n2x*ratio*dl*im2;
            rb2["velocity_y"] = vy2 + n2y*ratio*dl*im2;
        }

        // Breakable pulley
        accumulate_joint_impulse(e, n1x*dl, n1y*dl);
        check_and_break_joint(e, dt, [](Entity& j){ j["_broken"]=true; });
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑦  KINEMATIC MOVE POSITION / MOVE ROTATION
// ════════════════════════════════════════════════════════════════════════════
void move_position(Entity& entity, float wx, float wy, float dt) {
    if (!has_component(entity,"Transform")||!has_component(entity,"Rigidbody2D")) return;
    auto& tr = entity["components"]["Transform"];
    auto& rb = entity["components"]["Rigidbody2D"];
    float dt_safe = std::max(dt, 1e-9f);

    float cur_x = finite_val(get_float(tr,"x"));
    float cur_y = finite_val(get_float(tr,"y"));

    // Set implicit velocity so the body arrives at (wx,wy) after dt
    rb["velocity_x"] = (wx - cur_x) / dt_safe;
    rb["velocity_y"] = (wy - cur_y) / dt_safe;

    // For kinematic bodies, directly set position too (so queries see updated pos)
    if (body_type(rb) == "kinematic") {
        tr["x"] = wx;
        tr["y"] = wy;
        transform::mark_local_dirty(entity.value("id",0));
    }
}

void move_rotation(Entity& entity, float target_angle_deg, float dt) {
    if (!has_component(entity,"Transform")||!has_component(entity,"Rigidbody2D")) return;
    auto& tr = entity["components"]["Transform"];
    auto& rb = entity["components"]["Rigidbody2D"];
    float dt_safe = std::max(dt, 1e-9f);

    float cur = finite_val(get_float(tr,"rotation"));
    // Shortest angle difference
    float diff = target_angle_deg - cur;
    while (diff >  180.f) diff -= 360.f;
    while (diff < -180.f) diff += 360.f;

    // rad/s
    float omega = (diff * (float)M_PI / 180.f) / dt_safe;
    rb["angular_velocity"] = omega;

    if (body_type(rb) == "kinematic") {
        tr["rotation"] = target_angle_deg;
        transform::mark_local_dirty(entity.value("id",0));
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑧  SHAPE CASTS
// ════════════════════════════════════════════════════════════════════════════
// Internal: sweep a moving shape along (dx,dy)*distance against the world.
// We discretize the sweep into steps and call dispatch_shapes at each.
// For production use a proper TOI sweep; this is a robust iterative approach
// good enough for game-quality shape casting.

namespace {
    // Build a Shape for the sweep test (at offset t along direction)
    Shape make_circle_shape_at(float cx, float cy, float r) {
        Shape s; s.kind=ShapeKind::Circle; s.cx=cx; s.cy=cy; s.radius=r; return s;
    }
    Shape make_box_shape_at(float cx, float cy, float hw, float hh, float rot_deg) {
        float rot=rot_deg*(float)M_PI/180.f, c=std::cos(rot), s_=std::sin(rot);
        Verts verts;
        for(auto [dx,dy]:std::initializer_list<Vec2>{{-hw,-hh},{hw,-hh},{hw,hh},{-hw,hh}})
            verts.push_back({cx+dx*c-dy*s_, cy+dx*s_+dy*c});
        Shape sh; sh.kind=ShapeKind::Polygon; sh.verts=ensure_ccw(verts); sh.cx=cx; sh.cy=cy; return sh;
    }
    Shape make_capsule_shape_at(float cx, float cy, float r, float hh, bool horiz) {
        Shape sh; sh.kind=ShapeKind::Capsule; sh.cx=cx; sh.cy=cy; sh.radius=r; sh.cap_half_h=hh; sh.cap_horiz=horiz;
        if(horiz){ sh.world_pts.push_back({cx-hh,cy}); sh.world_pts.push_back({cx+hh,cy}); }
        else     { sh.world_pts.push_back({cx,cy-hh}); sh.world_pts.push_back({cx,cy+hh}); }
        return sh;
    }

    using ShapeFactory = std::function<Shape(float cx, float cy)>;

    std::vector<ShapeCastHit> shape_cast_impl(
        EntityList& entities,
        float ox, float oy,
        const ShapeFactory& make_shape,
        float dx, float dy,
        float distance,
        int layer_mask,
        bool query_triggers,
        bool first_only)
    {
        float len = std::hypot(dx,dy);
        if (len < 1e-12f) return {};
        float nx = dx/len, ny = dy/len;
        float max_dist = std::min(distance, len < 1e-9f ? distance : distance);

        // Use 12 sweep steps; refine via binary search on hit.
        const int STEPS = 12;
        std::vector<ShapeCastHit> hits;
        std::unordered_set<int> already_hit;

        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (has_component(e,"Rigidbody2D")) {
                int lay = e["components"]["Rigidbody2D"].value("layer",0);
                if (!(layer_mask & (1<<lay))) continue;
            }
            auto world_shapes = collect_shapes(e);
            for (auto& ws : world_shapes) {
                if (!query_triggers && ws.col && ws.col->value("is_trigger",false)) continue;

                // Binary-search TOI: find smallest t in [0,max_dist] where swept shape hits ws
                float lo=0.f, hi=max_dist;
                bool found=false;
                // Quick reject: test at t=max_dist
                {
                    Shape q = make_shape(ox+nx*max_dist, oy+ny*max_dist);
                    auto m = dispatch_shapes(q, const_cast<Shape&>(ws));
                    if(!m || m->contact_count==0) continue; // no hit along entire sweep
                }
                // Refine
                for(int iter=0;iter<12;++iter){
                    float mid=(lo+hi)*0.5f;
                    Shape q = make_shape(ox+nx*mid, oy+ny*mid);
                    auto m = dispatch_shapes(q, const_cast<Shape&>(ws));
                    if(m && m->contact_count>0) { hi=mid; found=true; }
                    else                        { lo=mid; }
                    if(hi-lo < 0.05f) break;
                }
                if(!found) {
                    // Might be overlapping at t=0
                    Shape q0 = make_shape(ox, oy);
                    auto m0 = dispatch_shapes(q0, const_cast<Shape&>(ws));
                    if(!m0||m0->contact_count==0) continue;
                    hi=0.f;
                }

                int eid = e.value("id",0);
                if (already_hit.count(eid) && first_only) continue;
                already_hit.insert(eid);

                // Build hit from manifold at t=hi
                Shape qh = make_shape(ox+nx*hi, oy+ny*hi);
                auto mh = dispatch_shapes(qh, const_cast<Shape&>(ws));
                if(!mh || mh->contact_count==0) { mh = {Manifold{}}; mh->nx=0; mh->ny=-1; }
                ShapeCastHit sh;
                sh.distance  = hi;
                sh.normal    = {mh->nx, mh->ny};
                sh.centroid  = {ox+nx*hi, oy+ny*hi};
                sh.point     = {mh->contacts[0].x, mh->contacts[0].y};
                sh.entity    = &e;
                sh.collider  = ws.col;
                hits.push_back(sh);
                if(first_only) break;
            }
            if(first_only && !hits.empty()) break;
        }
        // Sort by distance
        std::sort(hits.begin(),hits.end(),[](auto& a,auto& b){return a.distance<b.distance;});
        return hits;
    }
} // anon

std::optional<ShapeCastHit> circle_cast(EntityList& entities,
    float ox, float oy, float radius, float dx, float dy,
    float distance, int layer_mask, bool query_triggers)
{
    auto hits = shape_cast_impl(entities, ox, oy,
        [radius](float cx,float cy){ return make_circle_shape_at(cx,cy,radius); },
        dx,dy,distance,layer_mask,query_triggers,true);
    if(hits.empty()) return std::nullopt;
    return hits[0];
}
std::vector<ShapeCastHit> circle_cast_all(EntityList& entities,
    float ox, float oy, float radius, float dx, float dy,
    float distance, int layer_mask, bool query_triggers)
{
    return shape_cast_impl(entities,ox,oy,
        [radius](float cx,float cy){ return make_circle_shape_at(cx,cy,radius); },
        dx,dy,distance,layer_mask,query_triggers,false);
}
std::optional<ShapeCastHit> box_cast(EntityList& entities,
    float ox, float oy, float w, float h, float rot_deg,
    float dx, float dy, float distance, int layer_mask, bool query_triggers)
{
    float hw=w*0.5f,hh=h*0.5f;
    auto hits = shape_cast_impl(entities,ox,oy,
        [hw,hh,rot_deg](float cx,float cy){ return make_box_shape_at(cx,cy,hw,hh,rot_deg); },
        dx,dy,distance,layer_mask,query_triggers,true);
    if(hits.empty()) return std::nullopt;
    return hits[0];
}
std::vector<ShapeCastHit> box_cast_all(EntityList& entities,
    float ox, float oy, float w, float h, float rot_deg,
    float dx, float dy, float distance, int layer_mask, bool query_triggers)
{
    float hw=w*0.5f,hh=h*0.5f;
    return shape_cast_impl(entities,ox,oy,
        [hw,hh,rot_deg](float cx,float cy){ return make_box_shape_at(cx,cy,hw,hh,rot_deg); },
        dx,dy,distance,layer_mask,query_triggers,false);
}
std::optional<ShapeCastHit> capsule_cast(EntityList& entities,
    float ox, float oy, float radius, float half_h, bool horizontal,
    float dx, float dy, float distance, int layer_mask, bool query_triggers)
{
    auto hits = shape_cast_impl(entities,ox,oy,
        [radius,half_h,horizontal](float cx,float cy){
            return make_capsule_shape_at(cx,cy,radius,half_h,horizontal); },
        dx,dy,distance,layer_mask,query_triggers,true);
    if(hits.empty()) return std::nullopt;
    return hits[0];
}
std::vector<ShapeCastHit> capsule_cast_all(EntityList& entities,
    float ox, float oy, float radius, float half_h, bool horizontal,
    float dx, float dy, float distance, int layer_mask, bool query_triggers)
{
    return shape_cast_impl(entities,ox,oy,
        [radius,half_h,horizontal](float cx,float cy){
            return make_capsule_shape_at(cx,cy,radius,half_h,horizontal); },
        dx,dy,distance,layer_mask,query_triggers,false);
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑨  CONTACT FILTER 2D
// ════════════════════════════════════════════════════════════════════════════
bool ContactFilter2D::accepts(const Manifold& m) const {
    // Trigger filter
    if (!use_triggers && m.is_trigger) return false;
    // Layer mask: either entity's layer must pass
    if (use_layer_mask) {
        auto check_layer = [&](Entity* e) -> bool {
            if (!e) return false;
            int lay = has_component(*e,"Rigidbody2D") ? (*e)["components"]["Rigidbody2D"].value("layer",0) : 0;
            return (layer_mask & (1<<lay)) != 0;
        };
        if (!check_layer(m.e1) && !check_layer(m.e2)) return false;
    }
    // Normal angle filter
    if (use_normal_angle) {
        float angle_deg = std::atan2(m.ny, m.nx) * (180.f/(float)M_PI);
        if (angle_deg < 0.f) angle_deg += 360.f;
        if (angle_deg < min_normal_angle || angle_deg > max_normal_angle) return false;
    }
    // Custom predicate
    if (custom_predicate) {
        if (m.e1 && !custom_predicate(m.e1)) return false;
        if (m.e2 && !custom_predicate(m.e2)) return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑩  BREAKABLE JOINTS — proper impulse accumulation
// ════════════════════════════════════════════════════════════════════════════
void set_joint_break_listener(std::function<void(Entity* joint_entity, float break_force)> cb) {
    s_joint_break_cb = std::move(cb);
}

void accumulate_joint_impulse(Entity& joint_entity, float jx, float jy, float j_angular) {
    float prev_x = finite_val(joint_entity.value("_acc_imp_x",0.f));
    float prev_y = finite_val(joint_entity.value("_acc_imp_y",0.f));
    float prev_a = finite_val(joint_entity.value("_acc_imp_a",0.f));
    joint_entity["_acc_imp_x"] = prev_x + jx;
    joint_entity["_acc_imp_y"] = prev_y + jy;
    joint_entity["_acc_imp_a"] = prev_a + j_angular;
}

bool check_and_break_joint(Entity& joint_entity, float dt,
                           std::function<void(Entity&)> on_break)
{
    if (joint_entity.value("_broken", false)) return true;
    float break_force  = finite_val(joint_entity.value("break_force",  BREAK_FORCE_DEFAULT));
    float break_torque = finite_val(joint_entity.value("break_torque", BREAK_TORQUE_DEFAULT));

    float jx = finite_val(joint_entity.value("_acc_imp_x",0.f));
    float jy = finite_val(joint_entity.value("_acc_imp_y",0.f));
    float ja = finite_val(joint_entity.value("_acc_imp_a",0.f));

    float dt_safe = std::max(dt, 1e-9f);
    float force_mag  = std::hypot(jx,jy) / dt_safe;
    float torque_mag = std::abs(ja)       / dt_safe;

    bool broken = (force_mag > break_force) || (torque_mag > break_torque);
    if (broken) {
        joint_entity["_broken"] = true;
        if (on_break) on_break(joint_entity);
        if (s_joint_break_cb) s_joint_break_cb(&joint_entity, force_mag);
    }
    // Reset accumulator every step
    joint_entity["_acc_imp_x"] = 0.f;
    joint_entity["_acc_imp_y"] = 0.f;
    joint_entity["_acc_imp_a"] = 0.f;
    return broken;
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑪  PHYSICS MATERIAL RUNTIME API
// ════════════════════════════════════════════════════════════════════════════
static const char* _col_component(const std::string& t) { return t.c_str(); }

void set_collider_material(Entity& entity, const PhysicsMaterial& mat,
                           const std::string& collider_type)
{
    if (!has_component(entity, collider_type)) return;
    auto& col = entity["components"][collider_type];
    col["friction"]   = mat.friction;
    col["bounciness"] = mat.bounciness;
    auto mode_str = [](CombineMode m) -> std::string {
        switch(m){
            case CombineMode::Minimum:  return "minimum";
            case CombineMode::Maximum:  return "maximum";
            case CombineMode::Multiply: return "multiply";
            default:                    return "average";
        }
    };
    col["friction_combine"] = mode_str(mat.friction_combine);
    col["bounce_combine"]   = mode_str(mat.bounce_combine);
}

PhysicsMaterial get_collider_material(const Entity& entity, const std::string& collider_type) {
    PhysicsMaterial m;
    if (!has_component(entity, collider_type)) return m;
    const auto& col = entity["components"][collider_type];
    m.friction   = col.value("friction",   0.4f);
    m.bounciness = col.value("bounciness", 0.0f);
    auto str_mode = [](const std::string& s, CombineMode def) -> CombineMode {
        if(s=="minimum")  return CombineMode::Minimum;
        if(s=="maximum")  return CombineMode::Maximum;
        if(s=="multiply") return CombineMode::Multiply;
        return def;
    };
    m.friction_combine = str_mode(col.value("friction_combine",std::string{}), CombineMode::Average);
    m.bounce_combine   = str_mode(col.value("bounce_combine",  std::string{}), CombineMode::Maximum);
    return m;
}

void set_friction(Entity& entity, float friction, const std::string& col) {
    if (!has_component(entity,col)) return;
    entity["components"][col]["friction"] = friction;
}
void set_bounciness(Entity& entity, float bounciness, const std::string& col) {
    if (!has_component(entity,col)) return;
    entity["components"][col]["bounciness"] = bounciness;
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑫  OVERLAP QUERIES WITH CONTACT INFO
// ════════════════════════════════════════════════════════════════════════════
std::vector<OverlapHit> overlap_circle_ex(EntityList& entities,
    float x, float y, float r, const ContactFilter2D& filter)
{
    Shape q = make_circle_shape_at(x,y,r);
    std::vector<OverlapHit> hits;
    for(auto& e : entities){
        if(!entity_active(e)) continue;
        if(filter.use_layer_mask && has_component(e,"Rigidbody2D")){
            int lay=e["components"]["Rigidbody2D"].value("layer",0);
            if(!(filter.layer_mask&(1<<lay))) continue;
        }
        auto shapes=collect_shapes(e);
        for(auto& ws : shapes){
            if(!filter.use_triggers && ws.col && ws.col->value("is_trigger",false)) continue;
            Shape qc=q;
            auto m=dispatch_shapes(qc, const_cast<Shape&>(ws));
            if(!m||m->contact_count==0) continue;
            OverlapHit oh; oh.entity=&e; oh.collider=ws.col;
            oh.normal={m->nx,m->ny};
            oh.depth=m->contacts[0].depth;
            hits.push_back(oh);
            break;
        }
    }
    return hits;
}

std::vector<OverlapHit> overlap_box_ex(EntityList& entities,
    float x, float y, float w, float h, float rot_deg, const ContactFilter2D& filter)
{
    Shape q = make_box_shape_at(x,y,w*0.5f,h*0.5f,rot_deg);
    std::vector<OverlapHit> hits;
    for(auto& e : entities){
        if(!entity_active(e)) continue;
        if(filter.use_layer_mask && has_component(e,"Rigidbody2D")){
            int lay=e["components"]["Rigidbody2D"].value("layer",0);
            if(!(filter.layer_mask&(1<<lay))) continue;
        }
        auto shapes=collect_shapes(e);
        for(auto& ws : shapes){
            if(!filter.use_triggers && ws.col && ws.col->value("is_trigger",false)) continue;
            Shape qc=q;
            auto m=dispatch_shapes(qc,const_cast<Shape&>(ws));
            if(!m||m->contact_count==0) continue;
            OverlapHit oh; oh.entity=&e; oh.collider=ws.col;
            oh.normal={m->nx,m->ny}; oh.depth=m->contacts[0].depth;
            hits.push_back(oh); break;
        }
    }
    return hits;
}

std::vector<OverlapHit> overlap_capsule_ex(EntityList& entities,
    float x, float y, float radius, float half_h, bool horizontal,
    const ContactFilter2D& filter)
{
    Shape q = make_capsule_shape_at(x,y,radius,half_h,horizontal);
    std::vector<OverlapHit> hits;
    for(auto& e : entities){
        if(!entity_active(e)) continue;
        if(filter.use_layer_mask && has_component(e,"Rigidbody2D")){
            int lay=e["components"]["Rigidbody2D"].value("layer",0);
            if(!(filter.layer_mask&(1<<lay))) continue;
        }
        auto shapes=collect_shapes(e);
        for(auto& ws : shapes){
            if(!filter.use_triggers && ws.col && ws.col->value("is_trigger",false)) continue;
            Shape qc=q;
            auto m=dispatch_shapes(qc,const_cast<Shape&>(ws));
            if(!m||m->contact_count==0) continue;
            OverlapHit oh; oh.entity=&e; oh.collider=ws.col;
            oh.normal={m->nx,m->ny}; oh.depth=m->contacts[0].depth;
            hits.push_back(oh); break;
        }
    }
    return hits;
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑬  GET CONTACTS FOR ENTITY
// ════════════════════════════════════════════════════════════════════════════
const std::vector<Manifold>& get_last_manifolds() { return s_last_manifolds; }

std::vector<const Manifold*> get_contacts(const Entity& entity) {
    int eid = entity.value("id",0);
    std::vector<const Manifold*> out;
    for(auto& m : s_last_manifolds){
        if((m.e1 && m.e1->value("id",0)==eid) ||
           (m.e2 && m.e2->value("id",0)==eid))
            out.push_back(&m);
    }
    return out;
}

int get_contact_count(const Entity& entity) {
    return (int)get_contacts(entity).size();
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑭  RIGIDBODY HELPERS
// ════════════════════════════════════════════════════════════════════════════
static float _body_angle_rad(const Entity& entity) {
    if(!has_component(entity,"Transform")) return 0.f;
    return finite_val(get_float(entity["components"]["Transform"],"rotation"))
           * (float)M_PI/180.f;
}

Vec2 get_relative_point(const Entity& entity, float wx, float wy) {
    if(!has_component(entity,"Transform")) return {wx,wy};
    auto& tr = entity["components"]["Transform"];
    float tx=finite_val(get_float(tr,"x")), ty=finite_val(get_float(tr,"y"));
    float a=_body_angle_rad(entity);
    float c=std::cos(-a), s=std::sin(-a); // inverse rotation
    float lx=(wx-tx)*c-(wy-ty)*s;
    float ly=(wx-tx)*s+(wy-ty)*c;
    return {lx,ly};
}

Vec2 get_point(const Entity& entity, float lx, float ly) {
    if(!has_component(entity,"Transform")) return {lx,ly};
    auto& tr = entity["components"]["Transform"];
    float tx=finite_val(get_float(tr,"x")), ty=finite_val(get_float(tr,"y"));
    float a=_body_angle_rad(entity);
    float c=std::cos(a), s=std::sin(a);
    return {tx + lx*c - ly*s, ty + lx*s + ly*c};
}

Vec2 get_relative_vector(const Entity& entity, float wx, float wy) {
    float a=_body_angle_rad(entity);
    float c=std::cos(-a), s=std::sin(-a);
    return {wx*c - wy*s, wx*s + wy*c};
}

Vec2 get_point_velocity(const Entity& entity, float wx, float wy) {
    if(!has_component(entity,"Rigidbody2D")||!has_component(entity,"Transform")) return {0,0};
    auto& rb = entity["components"]["Rigidbody2D"];
    auto& tr = entity["components"]["Transform"];
    float vx=finite_val(rb.value("velocity_x",0.f));
    float vy=finite_val(rb.value("velocity_y",0.f));
    float av=finite_val(rb.value("angular_velocity",0.f)); // rad/s
    float tx=finite_val(get_float(tr,"x")), ty=finite_val(get_float(tr,"y"));
    float rx=wx-tx, ry=wy-ty;
    // v_point = v_cm + omega × r = (vx - av*ry, vy + av*rx)
    return {vx - av*ry, vy + av*rx};
}

bool is_sleeping(const Entity& entity) {
    if(!has_component(entity,"Rigidbody2D")) return false;
    return entity["components"]["Rigidbody2D"].value("_sleeping",false);
}

void wake_up(Entity& entity) {
    if(!has_component(entity,"Rigidbody2D")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    rb["_sleeping"]=false;
    rb["_sleep_t"]=0.f;
    rb["vx"]=(float)rb["velocity_x"];
    rb["vy"]=(float)rb["velocity_y"];
}

void sleep(Entity& entity) {
    if(!has_component(entity,"Rigidbody2D")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    rb["_sleeping"]=true;
    rb["velocity_x"]=0.f; rb["velocity_y"]=0.f; rb["vx"]=0.f; rb["vy"]=0.f; rb["angular_velocity"]=0.f;
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑮  CUSTOM GRAVITY PER-BODY
// ════════════════════════════════════════════════════════════════════════════
void set_body_gravity(Entity& entity, float gx, float gy) {
    if(!has_component(entity,"Rigidbody2D")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    rb["_custom_grav_x"]=gx;
    rb["_custom_grav_y"]=gy;
    rb["_has_custom_grav"]=true;
}

void clear_body_gravity(Entity& entity) {
    if(!has_component(entity,"Rigidbody2D")) return;
    auto& rb=entity["components"]["Rigidbody2D"];
    if(rb.contains("_custom_grav_x")) rb.erase("_custom_grav_x");
    if(rb.contains("_custom_grav_y")) rb.erase("_custom_grav_y");
    if(rb.contains("_has_custom_grav")) rb.erase("_has_custom_grav");
}

// Helper: inject custom gravity into _grav_x/_grav_y so integrate() picks it up
static void inject_custom_gravities(EntityList& entities) {
    Vec2 gv = get_gravity_vector(); // respect global gravity direction
    for(auto& e : entities){
        if(!has_component(e,"Rigidbody2D")) continue;
        auto& rb=e["components"]["Rigidbody2D"];
        if(rb.value("_has_custom_grav",false)){
            // Override the per-frame gravity inject
            rb["_grav_x"]=finite_val(rb.value("_custom_grav_x",0.f));
            rb["_grav_y"]=finite_val(rb.value("_custom_grav_y",gv.second));
        } else {
            // Inject global gravity direction * global magnitude
            float scale = finite_val(rb.value("gravity_scale",1.f),1.f);
            rb["_grav_x"] = gv.first  * scale;
            rb["_grav_y"] = gv.second * scale;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  APPLY_PHYSICS_EXT — drop-in replacement
// ════════════════════════════════════════════════════════════════════════════
void apply_physics_ext(EntityList& entities, float dt, float gravity)
{
    // ── Expose active list to threshold/global setters (㉛/㊸) ────────────────
    s_active_entities_ext = &entities;

    // ── Enforce configurable global speed caps (㊸) ─────────────────────────
    clamp_body_speeds(entities);

    // ── Pre-step effectors ──────────────────────────────────────────────────
    apply_constant_force2d(entities);
    apply_buoyancy_effectors(entities, gravity);
    apply_point_effectors2(entities);
    apply_area_effectors2(entities);
    if (s_magnus_enabled) apply_magnus_effect(entities);
    // Shape-aware drag (Phase 3): per-body opt-in via rb["shape_drag"]=true.
    // Was implemented but never invoked from the main loop — wiring it in
    // here means bodies marked shape_drag now actually get a cross-section
    // -proportional drag force each step instead of falling back silently
    // to the uniform rb.drag scalar.
    apply_shape_aware_drag(entities);

    // ── Custom gravity injection ─────────────────────────────────────────────
    inject_custom_gravities(entities);

    // ── Build entity map for joint systems ──────────────────────────────────
    std::unordered_map<int,Entity*> emap;
    for(auto& e : entities) emap[e.value("id",0)]=&e;

    // ── Pulley joints (before main solve so they contribute to velocity) ────
    solve_pulley_joints(entities, dt, emap);

    // ── Core physics step ────────────────────────────────────────────────────
    apply_physics(entities, dt, gravity);

    // ── Store manifolds for get_contacts() ──────────────────────────────────
    // We can't directly access the internal manifold list from apply_physics,
    // so we re-run a lightweight narrow phase query here to populate
    // s_last_manifolds for the contact query API.
    // NOTE: this is informational only (read-only copy); the actual solving
    // already happened inside apply_physics().
    {
        // Collect broad+narrow without solving (for contact query API).
        // We skip this expensive re-run if no one called get_contacts/get_last_manifolds
        // by checking if anyone registered for the feature.
        // For lightweight engines: comment out this block if not needed.
        s_last_manifolds.clear();
        // Re-run narrow phase to populate manifold list (read-only; no impulse applied)
        // This reuses the public collect_shapes / dispatch_shapes path.
        struct Pair { Entity* e1; Entity* e2; };
        std::unordered_set<long long> seen;
        for(auto& e1 : entities){
            if(!entity_active(e1)) continue;
            auto sh1 = collect_shapes(e1);
            for(auto& e2 : entities){
                if(&e1==&e2||!entity_active(e2)) continue;
                int id1=e1.value("id",0),id2=e2.value("id",0);
                int lo=std::min(id1,id2),hi=std::max(id1,id2);
                long long key=((long long)lo<<32)|(unsigned)hi;
                if(seen.count(key)) continue; seen.insert(key);
                auto sh2=collect_shapes(e2);
                for(auto& s1:sh1) for(auto& s2:sh2){
                    Shape ms1=s1,ms2=s2;
                    auto m=dispatch_shapes(ms1,ms2);
                    if(!m||m->contact_count==0) continue;
                    m->e1=const_cast<Entity*>(&e1);
                    m->e2=const_cast<Entity*>(&e2);
                    m->col1=s1.col; m->col2=s2.col;
                    m->rb1=has_component(e1,"Rigidbody2D")?&e1["components"]["Rigidbody2D"]:nullptr;
                    m->rb2=has_component(e2,"Rigidbody2D")?&e2["components"]["Rigidbody2D"]:nullptr;
                    m->is_trigger=(s1.col&&s1.col->value("is_trigger",false))||
                                  (s2.col&&s2.col->value("is_trigger",false));
                    s_last_manifolds.push_back(*m);
                }
            }
        }
        // ── Rebuild per-entity contact index (O(1) get_contacts_fast) ────────
        rebuild_contact_index(s_last_manifolds);
        // ── Apply platform side-friction clamping ─────────────────────────────
        apply_platform_side_friction(s_last_manifolds, entities);
    }

    // ── Post-step joint break checks ─────────────────────────────────────────
    for(auto& e : entities){
        if(!entity_active(e)) continue;
        // Check all joint types for accumulated break force
        static const std::array<const char*,10> JOINT_TYPES = {
            "DistanceJoint2D","SpringJoint2D","HingeJoint2D","SliderJoint2D",
            "WheelJoint2D","GearJoint2D","RelativeJoint2D","FrictionJoint2D",
            "RopeJoint2D","PulleyJoint2D"
        };
        for(auto* jt : JOINT_TYPES){
            if(!has_component(e,jt)) continue;
            auto& joint_comp = e["components"][jt];
            check_and_break_joint(joint_comp, dt);
        }
    }

    // ── Trigger/Collision stay callbacks (⑯) ──────────────────────────────
    dispatch_stay_callbacks();

    // ── Velocity constraints enforcement (㉔) ──────────────────────────────
    for(auto& e : entities){
        if(!entity_active(e)) continue;
        apply_constraints(e);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑯  TRIGGER STAY / COLLISION STAY CALLBACKS
// ════════════════════════════════════════════════════════════════════════════
// We maintain a set of pairs that were overlapping last frame.  Any pair that
// is still overlapping this frame fires the stay callback.  Pairs that appear
// for the first time are already handled by the core enter callbacks; pairs
// that disappear are already handled by exit callbacks.

static std::function<void(Entity*, Entity*, const Manifold&)> s_trigger_stay_cb;
static std::function<void(Entity*, Entity*, const Manifold&)> s_collision_stay_cb;

// Set of (id_lo, id_hi) pairs that were overlapping in the PREVIOUS frame.
static std::unordered_set<long long> s_prev_overlap_pairs;

void set_trigger_stay_listener(std::function<void(Entity*, Entity*, const Manifold&)> cb) {
    s_trigger_stay_cb = std::move(cb);
}
void set_collision_stay_listener(std::function<void(Entity*, Entity*, const Manifold&)> cb) {
    s_collision_stay_cb = std::move(cb);
}

// Called from apply_physics_ext after s_last_manifolds is populated.
static void dispatch_stay_callbacks() {
    if (!s_trigger_stay_cb && !s_collision_stay_cb) {
        s_prev_overlap_pairs.clear();
        return;
    }

    std::unordered_set<long long> current_pairs;
    for (const auto& m : s_last_manifolds) {
        if (!m.e1 || !m.e2 || m.contact_count == 0) continue;
        int id1 = m.e1->value("id", 0);
        int id2 = m.e2->value("id", 0);
        int lo = std::min(id1, id2), hi = std::max(id1, id2);
        long long key = ((long long)lo << 32) | (unsigned)hi;
        current_pairs.insert(key);

        // Only fire stay if the pair was ALSO touching last frame.
        if (s_prev_overlap_pairs.count(key)) {
            if (m.is_trigger && s_trigger_stay_cb)
                s_trigger_stay_cb(m.e1, m.e2, m);
            else if (!m.is_trigger && s_collision_stay_cb)
                s_collision_stay_cb(m.e1, m.e2, m);
        }
    }
    s_prev_overlap_pairs = std::move(current_pairs);
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑰  LINECAST
// ════════════════════════════════════════════════════════════════════════════
// A linecast is just a raycast whose direction and distance are derived from
// the two endpoints.  We delegate to the existing raycast() / raycast_all().

std::optional<RayHit> linecast(EntityList& entities,
                                float x1, float y1, float x2, float y2,
                                int layer_mask, bool query_triggers) {
    float dx = x2 - x1, dy = y2 - y1;
    float dist = std::hypot(dx, dy);
    if (dist < 1e-12f) return std::nullopt;
    return raycast(entities, x1, y1, dx / dist, dy / dist,
                   dist, layer_mask, query_triggers);
}

std::vector<RayHit> linecast_all(EntityList& entities,
                                  float x1, float y1, float x2, float y2,
                                  int layer_mask, bool query_triggers) {
    float dx = x2 - x1, dy = y2 - y1;
    float dist = std::hypot(dx, dy);
    if (dist < 1e-12f) return {};
    return raycast_all(entities, x1, y1, dx / dist, dy / dist,
                       dist, layer_mask, query_triggers);
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑱  OVERLAP POINT
// ════════════════════════════════════════════════════════════════════════════
// A zero-radius circle overlap is the canonical approach for point queries.

std::vector<Entity*> overlap_point(EntityList& entities,
                                    float px, float py,
                                    int layer_mask, bool query_triggers) {
    // Use a very small radius (epsilon) so the circle-overlap test works with
    // all collider types including polygons.
    return overlap_circle(entities, px, py, 0.5f, layer_mask);
    // Note: 0.5f pixel radius — small enough not to straddle boundaries in
    // typical 2D games where 1 unit ≥ 1 pixel.  For sub-pixel precision,
    // use overlap_point_ex() with a ContactFilter2D.
}

std::vector<OverlapHit> overlap_point_ex(EntityList& entities,
                                          float px, float py,
                                          const ContactFilter2D& filter) {
    return overlap_circle_ex(entities, px, py, 0.5f, filter);
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑲  TOTAL FORCE / TORQUE INSPECTION
// ════════════════════════════════════════════════════════════════════════════
// add_force() / apply_constant_force2d() accumulate into _acc_force_x/y and
// _acc_torque.  We expose those here.

Vec2 get_total_force(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return {0.f, 0.f};
    const auto& rb = entity["components"]["Rigidbody2D"];
    return { finite_val(rb.value("_acc_force_x", 0.f)),
             finite_val(rb.value("_acc_force_y", 0.f)) };
}

float get_total_torque(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return 0.f;
    const auto& rb = entity["components"]["Rigidbody2D"];
    return finite_val(rb.value("_acc_torque", 0.f));
}

void reset_forces(Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    rb["_acc_force_x"] = 0.f;
    rb["_acc_force_y"] = 0.f;
    rb["_acc_torque"]  = 0.f;
}

// ════════════════════════════════════════════════════════════════════════════
//  ⑳  PHYSICS_SIMULATE  (Physics2D.Simulate)
// ════════════════════════════════════════════════════════════════════════════
// Thin wrapper; apply_physics_ext already encapsulates the full pipeline.

void physics_simulate(EntityList& entities, float dt, float gravity) {
    apply_physics_ext(entities, dt, gravity);
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉑  NON-ALLOC QUERY VARIANTS
// ════════════════════════════════════════════════════════════════════════════

int overlap_circle_nonalloc(EntityList& entities,
                             float ox, float oy, float radius,
                             Entity** results, int buf_size,
                             int layer_mask, bool /*query_triggers*/) {
    auto hits = overlap_circle(entities, ox, oy, radius, layer_mask);
    int n = std::min((int)hits.size(), buf_size);
    for (int i = 0; i < n; ++i) results[i] = hits[i];
    return n;
}

int overlap_box_nonalloc(EntityList& entities,
                          float ox, float oy, float w, float h, float rot_deg,
                          Entity** results, int buf_size,
                          int layer_mask, bool /*query_triggers*/) {
    auto hits = overlap_box(entities, ox, oy, w, h, rot_deg, layer_mask);
    int n = std::min((int)hits.size(), buf_size);
    for (int i = 0; i < n; ++i) results[i] = hits[i];
    return n;
}

int raycast_nonalloc(EntityList& entities,
                      float ox, float oy, float dx, float dy, float distance,
                      RayHit* results, int buf_size,
                      int layer_mask, bool query_triggers) {
    auto hits = raycast_all(entities, ox, oy, dx, dy, distance, layer_mask, query_triggers);
    int n = std::min((int)hits.size(), buf_size);
    for (int i = 0; i < n; ++i) results[i] = hits[i];
    return n;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉒  COMPOSITE COLLIDER REBUILD
// ════════════════════════════════════════════════════════════════════════════
// Gathers all child collider shapes, optionally merges them into a convex hull
// (geometry_type = "polygons") or stores each outline separately ("outlines"),
// and writes the result into CompositeCollider2D["_merged_verts"].

void rebuild_composite_collider(Entity& entity) {
    if (!has_component(entity, "CompositeCollider2D")) return;
    auto& comp = entity["components"]["CompositeCollider2D"];

    std::string geo_type = comp.value("geometry_type", "polygons");
    float offset_dist    = finite_val(comp.value("offset_distance", 0.f));

    // Collect all shapes from this entity (multi-collider aware)
    auto shapes = collect_shapes(entity);
    if (shapes.empty()) return;

    // Gather all vertices from all shapes
    Verts all_pts;
    all_pts.reserve(shapes.size() * 8);
    for (auto& sh : shapes) {
        if (sh.kind == ShapeKind::Circle) {
            // Approximate circle with 12-gon
            constexpr int N = 12;
            for (int i = 0; i < N; ++i) {
                float a = (float)(2.0 * M_PI * i / N);
                all_pts.push_back({sh.cx + sh.radius * std::cos(a),
                                   sh.cy + sh.radius * std::sin(a)});
            }
        } else {
            for (auto& v : sh.verts) all_pts.push_back(v);
        }
    }

    Verts hull;
    if (geo_type == "polygons") {
        // Merge into a single convex hull
        hull = convex_hull(all_pts);
        // Apply offset (expand/contract each vertex along normal)
        if (std::abs(offset_dist) > 1e-6f && hull.size() >= 3) {
            // Compute centroid
            float cx = 0, cy = 0;
            for (auto& p : hull) { cx += p.first; cy += p.second; }
            cx /= (float)hull.size(); cy /= (float)hull.size();
            for (auto& p : hull) {
                float nx = p.first - cx, ny = p.second - cy;
                float d = std::hypot(nx, ny);
                if (d > 1e-9f) {
                    p.first  += nx / d * offset_dist;
                    p.second += ny / d * offset_dist;
                }
            }
        }
    } else {
        // "outlines" — keep all individual polygons (store first shape's hull only
        // in the merged field; multi-outline support is engine-specific)
        hull = all_pts;
    }

    // Write merged vertices back into component as JSON array
    auto arr = Entity::array();
    for (auto& p : hull) {
        auto pt = Entity::object();
        pt["x"] = p.first;
        pt["y"] = p.second;
        arr.push_back(pt);
    }
    comp["_merged_verts"] = arr;
    comp["_dirty"] = false;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉓  EFFECTOR PRIORITY & LAYER MASK
// ════════════════════════════════════════════════════════════════════════════

void set_effector_priority(Entity& entity, const std::string& effector_type, int priority) {
    if (!has_component(entity, effector_type)) return;
    entity["components"][effector_type]["_effector_priority"] = priority;
}

void set_effector_layer_mask(Entity& entity, const std::string& effector_type, int mask) {
    if (!has_component(entity, effector_type)) return;
    entity["components"][effector_type]["_effector_layer_mask"] = mask;
}

// Internal helper used by effector apply functions to honour priority.
// Returns true if the target entity's layer passes this effector's mask.
static bool effector_affects_layer(const Entity& effector_entity,
                                   const std::string& effector_type,
                                   const Entity& target) {
    if (!has_component(effector_entity, effector_type)) return false;
    const auto& ec = effector_entity["components"][effector_type];
    int mask = ec.value("_effector_layer_mask", 0xFFFF);
    if (mask == 0xFFFF) return true;
    int target_layer = 1;
    if (has_component(target, "Rigidbody2D"))
        target_layer = target["components"]["Rigidbody2D"].value("layer", 0);
    return (mask >> target_layer) & 1;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉔  VELOCITY CONSTRAINTS
// ════════════════════════════════════════════════════════════════════════════

void set_constraints(Entity& entity, int flags) {
    if (!has_component(entity, "Rigidbody2D")) return;
    entity["components"]["Rigidbody2D"]["_constraints"] = flags;
    // Sync freeze_rotation flag for compatibility with the core solver
    bool freeze_rot = (flags & FREEZE_ROTATION_EXT) != 0;
    entity["components"]["Rigidbody2D"]["freeze_rotation"] = freeze_rot;
}

int get_constraints(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return FREEZE_NONE;
    return entity["components"]["Rigidbody2D"].value("_constraints", FREEZE_NONE);
}

void apply_constraints(Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    int flags = rb.value("_constraints", FREEZE_NONE);
    if (flags == FREEZE_NONE) return;

    if (flags & FREEZE_POS_X) {
        rb["velocity_x"]         = 0.f;
        rb["_acc_force_x"]       = 0.f;
    }
    if (flags & FREEZE_POS_Y) {
        rb["velocity_y"]         = 0.f;
        rb["_acc_force_y"]       = 0.f;
    }
    if (flags & FREEZE_ROTATION_EXT) {
        rb["angular_velocity"]   = 0.f;
        rb["_acc_torque"]        = 0.f;
        rb["freeze_rotation"]    = true;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉕  CONTACT NORMAL / TANGENT IMPULSE QUERY
// ════════════════════════════════════════════════════════════════════════════
// s_last_manifolds is populated by apply_physics_ext() after each step.
// We scan it here to build the per-entity impulse list.

std::vector<ContactImpulse> get_contact_impulses(const Entity& entity) {
    std::vector<ContactImpulse> out;
    int eid = entity.value("id", 0);

    for (const auto& m : s_last_manifolds) {
        if (!m.e1 || !m.e2) continue;
        int id1 = m.e1->value("id", 0);
        int id2 = m.e2->value("id", 0);
        if (id1 != eid && id2 != eid) continue;

        bool is_a = (id1 == eid);
        Entity* other = is_a ? m.e2 : m.e1;
        // Normal points from B into A; flip for body B.
        float nx = is_a ?  m.nx : -m.nx;
        float ny = is_a ?  m.ny : -m.ny;

        for (int ci = 0; ci < m.contact_count; ++ci) {
            const auto& cp = m.contacts[ci];
            ContactImpulse ci_out;
            ci_out.point           = {cp.x, cp.y};
            ci_out.normal          = {nx, ny};
            ci_out.normal_impulse  = std::abs(cp.lambda_n);
            ci_out.tangent_impulse = std::abs(cp.lambda_t);
            ci_out.other           = other;
            out.push_back(ci_out);
        }
    }
    return out;
}


// ════════════════════════════════════════════════════════════════════════════
//  ㉖  VELOCITY SETTERS / GETTERS
// ════════════════════════════════════════════════════════════════════════════
void set_velocity(Entity& entity, float vx, float vy) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    rb["velocity_x"] = finite_val(vx);
    rb["velocity_y"] = finite_val(vy);
    rb["vx"] = finite_val(vx);
    rb["vy"] = finite_val(vy);
    rb["_sleeping"]  = false;
    rb["_sleep_t"]   = 0.f;
}

void set_angular_velocity(Entity& entity, float deg_per_sec) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    // Internal solver uses radians/s
    rb["angular_velocity"] = finite_val(deg_per_sec * (float)(M_PI / 180.0));
    rb["_sleeping"] = false;
    rb["_sleep_t"]  = 0.f;
}

float get_angular_velocity(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return 0.f;
    // Convert internal rad/s back to deg/s for Unity parity
    float av = finite_val(entity["components"]["Rigidbody2D"].value("angular_velocity", 0.f));
    return av * (float)(180.0 / M_PI);
}

void clamp_velocity(Entity& entity, float max_speed) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    float vx = finite_val(rb.contains("vx") ? (float)rb["vx"] : rb.value("velocity_x", 0.f));
    float vy = finite_val(rb.contains("vy") ? (float)rb["vy"] : rb.value("velocity_y", 0.f));
    float spd2 = vx * vx + vy * vy;
    float max2  = max_speed * max_speed;
    if (spd2 > max2 && spd2 > 1e-12f) {
        float s = max_speed / std::sqrt(spd2);
        rb["velocity_x"] = vx * s; rb["vx"] = vx * s;
        rb["velocity_y"] = vy * s; rb["vy"] = vy * s;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉗  RUNTIME BODY TYPE SWITCHING
// ════════════════════════════════════════════════════════════════════════════
void set_body_type(Entity& entity, BodyType2D type) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    switch (type) {
        case BodyType2D::Dynamic:
            rb["body_type"] = "dynamic";
            rb["_sleeping"] = false;
            break;
        case BodyType2D::Kinematic:
            rb["body_type"]        = "kinematic";
            rb["velocity_x"]       = 0.f;
            rb["velocity_y"]       = 0.f;
            rb["vx"]               = 0.f;
            rb["vy"]               = 0.f;
            rb["angular_velocity"] = 0.f;
            break;
        case BodyType2D::Static:
            rb["body_type"]        = "static";
            rb["velocity_x"]       = 0.f;
            rb["velocity_y"]       = 0.f;
            rb["vx"]               = 0.f;
            rb["vy"]               = 0.f;
            rb["angular_velocity"] = 0.f;
            rb["_sleeping"]        = true;
            break;
    }
}

BodyType2D get_body_type(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return BodyType2D::Static;
    std::string bt = body_type(entity["components"]["Rigidbody2D"]);
    if (bt == "dynamic")   return BodyType2D::Dynamic;
    if (bt == "kinematic") return BodyType2D::Kinematic;
    return BodyType2D::Static;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉘  CLOSEST POINT ON COLLIDER
// ════════════════════════════════════════════════════════════════════════════
Vec2 closest_point_on_collider(const Entity& entity, float wx, float wy) {
    auto shapes = collect_shapes(const_cast<Entity&>(entity));
    if (shapes.empty()) return {wx, wy};

    // For each shape find the closest surface point; return the globally closest.
    float best_dist2 = std::numeric_limits<float>::max();
    Vec2  best_pt    = {wx, wy};

    for (const auto& sh : shapes) {
        Vec2 cpt;
        if (sh.kind == ShapeKind::Circle) {
            // Closest point on circle circumference
            float dx = wx - sh.cx, dy = wy - sh.cy;
            float d  = std::hypot(dx, dy);
            if (d < 1e-9f) {
                cpt = {sh.cx + sh.radius, sh.cy};
            } else {
                cpt = {sh.cx + dx / d * sh.radius, sh.cy + dy / d * sh.radius};
            }
        } else if (sh.kind == ShapeKind::Capsule) {
            // Closest point on capsule surface = closest on segment + radius offset
            Vec2 A = {sh.cx, sh.cy - sh.cap_half_h};
            Vec2 B = {sh.cx, sh.cy + sh.cap_half_h};
            if (!sh.cap_horiz) {
                A = {sh.cx - sh.cap_half_h, sh.cy};
                B = {sh.cx + sh.cap_half_h, sh.cy};
            }
            // Project query point onto segment
            float abx = B.first - A.first, aby = B.second - A.second;
            float len2 = abx * abx + aby * aby;
            float seg_x, seg_y;
            if (len2 < 1e-12f) { seg_x = A.first; seg_y = A.second; }
            else {
                float t = std::max(0.f, std::min(1.f,
                    ((wx - A.first) * abx + (wy - A.second) * aby) / len2));
                seg_x = A.first + t * abx; seg_y = A.second + t * aby;
            }
            float dx = wx - seg_x, dy = wy - seg_y;
            float d  = std::hypot(dx, dy);
            if (d < 1e-9f) { cpt = {seg_x + sh.radius, seg_y}; }
            else            { cpt = {seg_x + dx / d * sh.radius, seg_y + dy / d * sh.radius}; }
        } else {
            // Polygon: find closest edge
            const auto& verts = sh.verts;
            int n = (int)verts.size();
            if (n == 0) continue;
            if (n == 1) { cpt = verts[0]; }
            else {
                float best_edge2 = std::numeric_limits<float>::max();
                cpt = verts[0];
                for (int i = 0; i < n; ++i) {
                    Vec2 A = verts[i], B = verts[(i + 1) % n];
                    float abx = B.first - A.first, aby = B.second - A.second;
                    float len2e = abx * abx + aby * aby;
                    float t = len2e < 1e-12f ? 0.f :
                        std::max(0.f, std::min(1.f,
                            ((wx - A.first) * abx + (wy - A.second) * aby) / len2e));
                    float px = A.first + t * abx, py = A.second + t * aby;
                    float d2 = (wx - px) * (wx - px) + (wy - py) * (wy - py);
                    if (d2 < best_edge2) { best_edge2 = d2; cpt = {px, py}; }
                }
            }
        }
        float d2 = (wx - cpt.first) * (wx - cpt.first) + (wy - cpt.second) * (wy - cpt.second);
        if (d2 < best_dist2) { best_dist2 = d2; best_pt = cpt; }
    }
    return best_pt;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉙  SHAPE DISTANCE
// ════════════════════════════════════════════════════════════════════════════
ShapeDistance shape_distance(const Entity& entity_a, const Entity& entity_b) {
    ShapeDistance result;
    result.distance   = std::numeric_limits<float>::max();
    result.overlapping = false;

    auto shapes_a = collect_shapes(const_cast<Entity&>(entity_a));
    auto shapes_b = collect_shapes(const_cast<Entity&>(entity_b));

    float best_dist = std::numeric_limits<float>::max();

    for (const auto& sa : shapes_a) {
        for (const auto& sb : shapes_b) {
            // Use SAT manifold to detect overlap
            Shape msa = sa, msb = sb;
            auto m = dispatch_shapes(msa, msb);
            if (m && m->contact_count > 0) {
                // Overlapping: distance is negative (penetration depth)
                float pen = -m->contacts[0].depth; // penetration expressed as negative distance
                if (pen < best_dist) {
                    best_dist           = pen;
                    result.distance     = pen;
                    result.overlapping  = true;
                    result.normal       = {m->nx, m->ny};
                    // Contact point is the midpoint of the deepest contact
                    auto& cp = m->contacts[0];
                    result.point_on_a   = {cp.x - m->nx * std::abs(pen) * 0.5f,
                                           cp.y - m->ny * std::abs(pen) * 0.5f};
                    result.point_on_b   = {cp.x + m->nx * std::abs(pen) * 0.5f,
                                           cp.y + m->ny * std::abs(pen) * 0.5f};
                }
            } else {
                // Non-overlapping: closest surface points
                // Sample centroids as query points for closest_point
                float cax = sa.cx, cay = sa.cy;
                float cbx = sb.cx, cby = sb.cy;
                Vec2 pa = closest_point_on_collider(entity_a, cbx, cby);
                Vec2 pb = closest_point_on_collider(entity_b, cax, cay);
                float dx = pb.first - pa.first, dy = pb.second - pa.second;
                float d = std::hypot(dx, dy);
                if (d < best_dist) {
                    best_dist          = d;
                    result.distance    = d;
                    result.overlapping = false;
                    result.point_on_a  = pa;
                    result.point_on_b  = pb;
                    result.normal      = d > 1e-9f ? Vec2{dx / d, dy / d} : Vec2{1.f, 0.f};
                }
            }
        }
    }
    return result;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉚  SWEEP TEST
// ════════════════════════════════════════════════════════════════════════════
std::optional<ShapeCastHit> sweep_test(Entity& entity, EntityList& world,
                                        float dx, float dy, float distance,
                                        int layer_mask, bool query_triggers) {
    // Use the entity's own shape to pick the right cast variant
    auto shapes = collect_shapes(entity);
    if (shapes.empty()) return std::nullopt;
    const auto& sh = shapes[0];
    float ox = sh.cx, oy = sh.cy;

    // Exclude the entity itself from world hits by temporarily tagging it,
    // then filter from results.
    std::optional<ShapeCastHit> best;

    auto filter_self = [&](const ShapeCastHit& h) {
        return h.entity != &entity;
    };

    if (sh.kind == ShapeKind::Circle) {
        auto hits = circle_cast_all(world, ox, oy, sh.radius, dx, dy, distance, layer_mask, query_triggers);
        for (auto& h : hits) if (filter_self(h)) { best = h; break; }
    } else if (sh.kind == ShapeKind::Capsule) {
        auto hits = capsule_cast_all(world, ox, oy, sh.radius, sh.cap_half_h,
                                     sh.cap_horiz, dx, dy, distance, layer_mask, query_triggers);
        for (auto& h : hits) if (filter_self(h)) { best = h; break; }
    } else {
        // OBB: use box_cast with shape AABB
        auto [bx1, by1, bx2, by2] = shape_aabb(sh);
        float w = bx2 - bx1, h = by2 - by1;
        float rot_deg = 0.f; // AABB cast; rotated polygon shapes use AABB approximation
        auto hits = box_cast_all(world, ox, oy, w, h, rot_deg, dx, dy, distance, layer_mask, query_triggers);
        for (auto& h : hits) if (filter_self(h)) { best = h; break; }
    }
    return best;
}

std::vector<ShapeCastHit> sweep_test_all(Entity& entity, EntityList& world,
                                          float dx, float dy, float distance,
                                          int layer_mask, bool query_triggers) {
    auto shapes = collect_shapes(entity);
    if (shapes.empty()) return {};
    const auto& sh = shapes[0];
    float ox = sh.cx, oy = sh.cy;

    std::vector<ShapeCastHit> hits;
    if (sh.kind == ShapeKind::Circle) {
        hits = circle_cast_all(world, ox, oy, sh.radius, dx, dy, distance, layer_mask, query_triggers);
    } else if (sh.kind == ShapeKind::Capsule) {
        hits = capsule_cast_all(world, ox, oy, sh.radius, sh.cap_half_h,
                                sh.cap_horiz, dx, dy, distance, layer_mask, query_triggers);
    } else {
        auto [bx1, by1, bx2, by2] = shape_aabb(sh);
        float w = bx2 - bx1, h = by2 - by1;
        hits = box_cast_all(world, ox, oy, w, h, 0.f, dx, dy, distance, layer_mask, query_triggers);
    }
    // Remove self-hits
    hits.erase(std::remove_if(hits.begin(), hits.end(),
        [&](const ShapeCastHit& h){ return h.entity == &entity; }), hits.end());
    return hits;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉛  SLEEP THRESHOLDS
// ════════════════════════════════════════════════════════════════════════════
// We store overrides in module-level statics.  The core physics.cpp reads
// SLEEP_VEL_SQ / SLEEP_ANG / SLEEP_TIME as constexpr from physics.hpp;
// the ext layer writes per-body "_sleep_vel_sq_override" / "_sleep_ang_override"
// OR uses global overrides injected via apply_physics_ext pre-step.

static float s_sleep_vel_threshold  = -1.f; // -1 = use engine default
static float s_sleep_ang_threshold  = -1.f;
static float s_sleep_time_threshold = -1.f;

void set_sleep_velocity_threshold(float pps) {
    s_sleep_vel_threshold = pps;
    // Broadcast to all active bodies so the core sleep check picks them up
    if (s_active_entities_ext) {
        for (auto& e : *s_active_entities_ext) {
            if (!has_component(e, "Rigidbody2D")) continue;
            e["components"]["Rigidbody2D"]["_sleep_vel_threshold"] = pps;
        }
    }
}
float get_sleep_velocity_threshold() { return s_sleep_vel_threshold; }

void set_sleep_angular_threshold(float dps) {
    s_sleep_ang_threshold = dps;
    if (s_active_entities_ext) {
        for (auto& e : *s_active_entities_ext) {
            if (!has_component(e, "Rigidbody2D")) continue;
            e["components"]["Rigidbody2D"]["_sleep_ang_threshold"] = dps * (float)(M_PI / 180.0);
        }
    }
}
float get_sleep_angular_threshold() { return s_sleep_ang_threshold; }

void set_sleep_time_threshold(float seconds) {
    s_sleep_time_threshold = seconds;
    if (s_active_entities_ext) {
        for (auto& e : *s_active_entities_ext) {
            if (!has_component(e, "Rigidbody2D")) continue;
            e["components"]["Rigidbody2D"]["_sleep_time_threshold"] = seconds;
        }
    }
}
float get_sleep_time_threshold() { return s_sleep_time_threshold; }

// ════════════════════════════════════════════════════════════════════════════
//  ㉜  PER-LAYER COLLISION IGNORE MATRIX
// ════════════════════════════════════════════════════════════════════════════
// 32×32 symmetric bitset (only upper triangle stored as 32 uint32_t bitmasks)
static uint32_t s_layer_ignore_matrix[32] = {};

void ignore_layer_collision(int la, int lb, bool ignore) {
    if (la < 0 || la >= 32 || lb < 0 || lb >= 32) return;
    if (la == lb) return;
    if (la > lb) std::swap(la, lb);
    if (ignore)
        s_layer_ignore_matrix[la] |=  (1u << lb);
    else
        s_layer_ignore_matrix[la] &= ~(1u << lb);
}

bool get_ignore_layer_collision(int la, int lb) {
    if (la < 0 || la >= 32 || lb < 0 || lb >= 32 || la == lb) return false;
    if (la > lb) std::swap(la, lb);
    return (s_layer_ignore_matrix[la] >> lb) & 1u;
}

// Called from apply_physics_ext pre-step to inject layer ignores into the
// collision ignore pair set so the core narrow phase skips them.
static void apply_layer_ignores(EntityList& entities) {
    // Build layer → entity list map
    std::unordered_map<int, std::vector<int>> layer_to_ids;
    std::unordered_map<int, Entity*>          id_to_entity;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        int eid = e.value("id", 0);
        id_to_entity[eid] = &e;
        int layer = 0;
        if (has_component(e, "Rigidbody2D"))
            layer = e["components"]["Rigidbody2D"].value("layer", 0);
        layer_to_ids[layer].push_back(eid);
    }
    // For each ignored layer pair, ignore_collision() for all cross-pair entities
    for (int la = 0; la < 32; ++la) {
        if (!layer_to_ids.count(la)) continue;
        for (int lb = la + 1; lb < 32; ++lb) {
            if (!layer_to_ids.count(lb)) continue;
            if (!get_ignore_layer_collision(la, lb)) continue;
            for (int ia : layer_to_ids[la])
                for (int ib : layer_to_ids[lb])
                    ignore_collision(ia, ib, true);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉝  FIXED JOINT 2D SOLVER
// ════════════════════════════════════════════════════════════════════════════
// Implements a soft/rigid weld: drives the relative position and angle between
// two bodies back to their initial attachment state.
void solve_fixed_joints(EntityList& entities, float dt,
                         std::unordered_map<int, Entity*>& emap) {
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e, "FixedJoint2D")) continue;
        if (!has_component(e, "Rigidbody2D") || !has_component(e, "Transform")) continue;

        auto& fj = e["components"]["FixedJoint2D"];
        if (fj.value("_broken", false)) continue;

        int  conn_id  = fj.value("connected_entity", -1);
        if (conn_id < 0 || !emap.count(conn_id)) continue;
        Entity* eb = emap[conn_id];
        if (!has_component(*eb, "Rigidbody2D") || !has_component(*eb, "Transform")) continue;

        auto& tA  = e[  "components"]["Transform"];
        auto& tB  = eb->operator[]("components")["Transform"];
        auto& rbA = e[  "components"]["Rigidbody2D"];
        auto& rbB = eb->operator[]("components")["Rigidbody2D"];

        float axw = get_float(tA, "x"), ayw = get_float(tA, "y");
        float bxw = get_float(tB, "x"), byw = get_float(tB, "y");
        float aRot = get_float(tA, "rotation") * (float)(M_PI / 180.0);
        float bRot = get_float(tB, "rotation") * (float)(M_PI / 180.0);

        // Initialise rest state on first solve
        if (!fj.value("_initialised", false)) {
            fj["_rest_dx"]   = bxw - axw;
            fj["_rest_dy"]   = byw - ayw;
            fj["_rest_drot"] = bRot - aRot;
            fj["_initialised"] = true;
        }

        float rest_dx   = finite_val(fj.value("_rest_dx",   0.f));
        float rest_dy   = finite_val(fj.value("_rest_dy",   0.f));
        float rest_drot = finite_val(fj.value("_rest_drot", 0.f));
        float freq      = finite_val(fj.value("frequency",  0.f));
        float damp      = finite_val(fj.value("damping_ratio", 0.f), 0.f);

        // Error: difference between current and rest relative position
        float err_x   = (bxw - axw) - rest_dx;
        float err_y   = (byw - ayw) - rest_dy;
        float err_rot = (bRot - aRot) - rest_drot;
        // Wrap angle error to [-π, π]
        while (err_rot >  (float)M_PI) err_rot -= 2.f * (float)M_PI;
        while (err_rot < -(float)M_PI) err_rot += 2.f * (float)M_PI;

        float imA = body_inv_mass(&rbA), imB = body_inv_mass(&rbB);
        float total_im = imA + imB;
        if (total_im < 1e-12f) continue;

        if (freq > 0.f) {
            // Soft weld: PD controller (spring-damper)
            float omega = 2.f * (float)M_PI * freq;
            float k  = omega * omega;
            float c  = 2.f * damp * omega;
            float vax = finite_val(rbA.value("velocity_x", 0.f));
            float vay = finite_val(rbA.value("velocity_y", 0.f));
            float vbx = finite_val(rbB.value("velocity_x", 0.f));
            float vby = finite_val(rbB.value("velocity_y", 0.f));
            float rel_vx = vbx - vax, rel_vy = vby - vay;

            float corr_x = -(k * err_x + c * rel_vx) * dt;
            float corr_y = -(k * err_y + c * rel_vy) * dt;
            float ratio  = imA / total_im;
            if (body_type(rbA) == "dynamic") {
                rbA["velocity_x"] = vax + corr_x * ratio;
                rbA["velocity_y"] = vay + corr_y * ratio;
            }
            if (body_type(rbB) == "dynamic") {
                rbB["velocity_x"] = vbx - corr_x * (1.f - ratio);
                rbB["velocity_y"] = vby - corr_y * (1.f - ratio);
            }
        } else {
            // Rigid weld: positional correction
            float bias = 0.2f / dt;
            float impulse_x = -bias * err_x / total_im;
            float impulse_y = -bias * err_y / total_im;
            if (body_type(rbA) == "dynamic") {
                rbA["velocity_x"] = (float)rbA["velocity_x"] - impulse_x * imA;
                rbA["velocity_y"] = (float)rbA["velocity_y"] - impulse_y * imA;
            }
            if (body_type(rbB) == "dynamic") {
                rbB["velocity_x"] = (float)rbB["velocity_x"] + impulse_x * imB;
                rbB["velocity_y"] = (float)rbB["velocity_y"] + impulse_y * imB;
            }
            // Angular correction
            if (!rbA.value("freeze_rotation", false) && body_type(rbA) == "dynamic") {
                float iA = std::max(finite_val(rbA.value("inertia", 1.f)), 1e-9f);
                rbA["angular_velocity"] = (float)rbA.value("angular_velocity", 0.f)
                                          - err_rot * bias * 0.5f / iA;
            }
            if (!rbB.value("freeze_rotation", false) && body_type(rbB) == "dynamic") {
                float iB = std::max(finite_val(rbB.value("inertia", 1.f)), 1e-9f);
                rbB["angular_velocity"] = (float)rbB.value("angular_velocity", 0.f)
                                          + err_rot * bias * 0.5f / iB;
            }
        }

        // Break check
        float fx = (bxw - axw) - rest_dx, fy = (byw - ayw) - rest_dy;
        accumulate_joint_impulse(e, fx, fy, err_rot);
        check_and_break_joint(e["components"]["FixedJoint2D"], dt);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉞  PER-ENTITY PHYSICS CALLBACKS
// ════════════════════════════════════════════════════════════════════════════
struct EntityCallbacks {
    PhysicsCallback on_collision_enter;
    PhysicsCallback on_collision_exit;
    PhysicsCallback on_collision_stay;
    PhysicsCallback on_trigger_enter;
    PhysicsCallback on_trigger_exit;
    PhysicsCallback on_trigger_stay;
};
static std::unordered_map<int, EntityCallbacks> s_entity_callbacks;

void register_collision_enter(Entity& entity, PhysicsCallback cb) {
    s_entity_callbacks[entity.value("id", 0)].on_collision_enter = std::move(cb);
}
void register_collision_exit(Entity& entity, PhysicsCallback cb) {
    s_entity_callbacks[entity.value("id", 0)].on_collision_exit = std::move(cb);
}
void register_collision_stay(Entity& entity, PhysicsCallback cb) {
    s_entity_callbacks[entity.value("id", 0)].on_collision_stay = std::move(cb);
}
void register_trigger_enter(Entity& entity, PhysicsCallback cb) {
    s_entity_callbacks[entity.value("id", 0)].on_trigger_enter = std::move(cb);
}
void register_trigger_exit(Entity& entity, PhysicsCallback cb) {
    s_entity_callbacks[entity.value("id", 0)].on_trigger_exit = std::move(cb);
}
void register_trigger_stay(Entity& entity, PhysicsCallback cb) {
    s_entity_callbacks[entity.value("id", 0)].on_trigger_stay = std::move(cb);
}
void unregister_physics_callbacks(Entity& entity) {
    s_entity_callbacks.erase(entity.value("id", 0));
}

// Dispatch per-entity enter/exit callbacks from the global ContactListener.
// Called at the end of apply_physics_ext after dispatch_stay_callbacks().
static void dispatch_per_entity_callbacks_enter_exit(EntityList& entities) {
    if (s_entity_callbacks.empty()) return;

    // Re-use s_last_manifolds which is already populated
    for (const auto& m : s_last_manifolds) {
        if (!m.e1 || !m.e2) continue;
        int id1 = m.e1->value("id", 0), id2 = m.e2->value("id", 0);
        int lo = std::min(id1, id2), hi = std::max(id1, id2);
        long long key = ((long long)lo << 32) | (unsigned)hi;
        bool was = s_prev_overlap_pairs.count(key) > 0;

        if (!was) {
            // Enter event — fire for both entities
            auto fire = [&](int eid, Entity* self, Entity* other) {
                auto it = s_entity_callbacks.find(eid);
                if (it == s_entity_callbacks.end()) return;
                if (m.is_trigger && it->second.on_trigger_enter)
                    it->second.on_trigger_enter(self, other, m);
                else if (!m.is_trigger && it->second.on_collision_enter)
                    it->second.on_collision_enter(self, other, m);
            };
            fire(id1, m.e1, m.e2);
            fire(id2, m.e2, m.e1);
        } else {
            // Stay event
            auto fire_stay = [&](int eid, Entity* self, Entity* other) {
                auto it = s_entity_callbacks.find(eid);
                if (it == s_entity_callbacks.end()) return;
                if (m.is_trigger && it->second.on_trigger_stay)
                    it->second.on_trigger_stay(self, other, m);
                else if (!m.is_trigger && it->second.on_collision_stay)
                    it->second.on_collision_stay(self, other, m);
            };
            fire_stay(id1, m.e1, m.e2);
            fire_stay(id2, m.e2, m.e1);
        }
    }

    // Exit: pairs in prev but not current
    // (s_prev_overlap_pairs is updated AFTER this call in dispatch_stay_callbacks;
    //  here we detect exits by scanning prev vs current_pairs)
    std::unordered_set<long long> current;
    for (const auto& m : s_last_manifolds)
        if (m.e1 && m.e2 && m.contact_count > 0) {
            int id1 = m.e1->value("id", 0), id2 = m.e2->value("id", 0);
            int lo = std::min(id1, id2), hi = std::max(id1, id2);
            current.insert(((long long)lo << 32) | (unsigned)hi);
        }

    // Build id→entity map for exit lookups
    std::unordered_map<int, Entity*> emap;
    for (auto& e : entities) emap[e.value("id", 0)] = &e;

    for (long long old_key : s_prev_overlap_pairs) {
        if (current.count(old_key)) continue;
        int lo = (int)(old_key >> 32), hi = (int)(old_key & 0xFFFFFFFF);
        auto* ea = emap.count(lo) ? emap[lo] : nullptr;
        auto* eb = emap.count(hi) ? emap[hi] : nullptr;
        if (!ea || !eb) continue;
        // We don't know is_trigger for the exited pair from the key alone;
        // fire both collision_exit and trigger_exit and let the user discriminate.
        Manifold dummy;
        auto fire_exit = [&](int eid, Entity* self, Entity* other) {
            auto it = s_entity_callbacks.find(eid);
            if (it == s_entity_callbacks.end()) return;
            if (it->second.on_collision_exit) it->second.on_collision_exit(self, other, dummy);
            if (it->second.on_trigger_exit)   it->second.on_trigger_exit(self, other, dummy);
        };
        fire_exit(lo, ea, eb);
        fire_exit(hi, eb, ea);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㉟  INTERPOLATION MODE
// ════════════════════════════════════════════════════════════════════════════
void set_interpolation_mode(Entity& entity, InterpolationMode mode) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    switch (mode) {
        case InterpolationMode::None:         rb["_interp_mode"] = 0; break;
        case InterpolationMode::Interpolate:  rb["_interp_mode"] = 1; break;
        case InterpolationMode::Extrapolate:  rb["_interp_mode"] = 2; break;
    }
    // enable_interpolation() in the core already stores prev state
    enable_interpolation(entity, mode != InterpolationMode::None);
}

InterpolationMode get_interpolation_mode(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return InterpolationMode::None;
    int m = entity["components"]["Rigidbody2D"].value("_interp_mode", 0);
    if (m == 1) return InterpolationMode::Interpolate;
    if (m == 2) return InterpolationMode::Extrapolate;
    return InterpolationMode::None;
}

Vec2 get_render_position(const Entity& entity, float alpha, float step_dt) {
    auto mode = get_interpolation_mode(entity);
    if (mode == InterpolationMode::Extrapolate && step_dt > 1e-9f) {
        // Extrapolate using current velocity
        Vec2 pos = {0.f, 0.f};
        if (has_component(entity, "Transform")) {
            pos.first  = get_float(entity["components"]["Transform"], "x");
            pos.second = get_float(entity["components"]["Transform"], "y");
        }
        if (has_component(entity, "Rigidbody2D")) {
            float vx = entity["components"]["Rigidbody2D"].value("velocity_x", 0.f);
            float vy = entity["components"]["Rigidbody2D"].value("velocity_y", 0.f);
            pos.first  += vx * alpha * step_dt;
            pos.second += vy * alpha * step_dt;
        }
        return pos;
    }
    // Interpolate (or None): delegate to core
    return get_interpolated_position(const_cast<Entity&>(entity), alpha);
}

float get_render_rotation(const Entity& entity, float alpha, float step_dt) {
    auto mode = get_interpolation_mode(entity);
    if (mode == InterpolationMode::Extrapolate && step_dt > 1e-9f) {
        float rot = 0.f;
        if (has_component(entity, "Transform"))
            rot = get_float(entity["components"]["Transform"], "rotation");
        if (has_component(entity, "Rigidbody2D")) {
            float av = entity["components"]["Rigidbody2D"].value("angular_velocity", 0.f);
            rot += av * alpha * step_dt * (float)(180.0 / M_PI);
        }
        return rot;
    }
    return get_interpolated_rotation(const_cast<Entity&>(entity), alpha);
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊱  TRAJECTORY PREDICTION
// ════════════════════════════════════════════════════════════════════════════
std::vector<TrajectoryPoint> predict_trajectory(const Entity& entity,
                                                 int steps, float step_dt,
                                                 float gravity) {
    std::vector<TrajectoryPoint> traj;
    traj.reserve(steps);
    if (!has_component(entity, "Rigidbody2D") || !has_component(entity, "Transform")) return traj;

    const auto& rb  = entity["components"]["Rigidbody2D"];
    const auto& tr  = entity["components"]["Transform"];

    float px = get_float(tr,  "x");
    float py = get_float(tr,  "y");
    float vx = finite_val(rb.value("velocity_x", 0.f));
    float vy = finite_val(rb.value("velocity_y", 0.f));
    float gs = finite_val(rb.value("gravity_scale", 1.f), 1.f);
    float drag = std::max(0.f, finite_val(rb.value("drag", 0.05f), 0.05f));

    // Use global gravity direction if set
    Vec2 gvec = get_gravity_vector();
    float gx_accel = gvec.first  * gs;
    float gy_accel = gvec.second * gs; // fallback to passed gravity if zero
    if (std::abs(gvec.first) < 1e-9f && std::abs(gvec.second) < 1e-9f)
        gy_accel = gravity * gs;

    float t = 0.f;
    for (int i = 0; i < steps; ++i) {
        traj.push_back({Vec2{px, py}, Vec2{vx, vy}, t});
        // Semi-implicit Euler (same as integrate())
        float damp = std::max(0.f, 1.f - drag * step_dt);
        vx = vx * damp + gx_accel * step_dt;
        vy = vy * damp + gy_accel * step_dt;
        px += vx * step_dt;
        py += vy * step_dt;
        t  += step_dt;
    }
    return traj;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊲  OVERLAP COLLIDER
// ════════════════════════════════════════════════════════════════════════════
std::vector<OverlapHit> overlap_collider(Entity& entity, EntityList& world,
                                          const ContactFilter2D& filter) {
    auto shapes = collect_shapes(entity);
    if (shapes.empty()) return {};

    std::vector<OverlapHit> out;
    for (const auto& sh : shapes) {
        std::vector<OverlapHit> hits;
        if (sh.kind == ShapeKind::Circle) {
            hits = overlap_circle_ex(world, sh.cx, sh.cy, sh.radius, filter);
        } else if (sh.kind == ShapeKind::Capsule) {
            hits = overlap_capsule_ex(world, sh.cx, sh.cy, sh.radius, sh.cap_half_h,
                                      sh.cap_horiz, filter);
        } else {
            auto [bx1, by1, bx2, by2] = shape_aabb(sh);
            float w = bx2 - bx1, h = by2 - by1;
            hits = overlap_box_ex(world, sh.cx, sh.cy, w, h, 0.f, filter);
        }
        // Remove self
        hits.erase(std::remove_if(hits.begin(), hits.end(),
            [&](const OverlapHit& h){ return h.entity == &entity; }), hits.end());
        out.insert(out.end(), hits.begin(), hits.end());
    }
    // Deduplicate by entity pointer
    std::sort(out.begin(), out.end(), [](const OverlapHit& a, const OverlapHit& b){
        return a.entity < b.entity;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const OverlapHit& a, const OverlapHit& b){
        return a.entity == b.entity;
    }), out.end());
    return out;
}

int overlap_collider_nonalloc(Entity& entity, EntityList& world,
                               Entity** results, int buf_size,
                               const ContactFilter2D& filter) {
    auto hits = overlap_collider(entity, world, filter);
    int n = std::min((int)hits.size(), buf_size);
    for (int i = 0; i < n; ++i) results[i] = hits[i].entity;
    return n;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊳  RUNTIME COLLIDER ATTACH / DETACH
// ════════════════════════════════════════════════════════════════════════════
static constexpr const char* COLLIDER_TYPES[] = {
    "BoxCollider2D", "CircleCollider2D", "CapsuleCollider2D",
    "PolygonCollider2D", "EdgeCollider2D", "CompositeCollider2D"
};

void attach_collider(Entity& entity, const std::string& collider_type,
                     const Entity& collider_data) {
    entity["components"][collider_type] = collider_data;
    // Wake the body so it re-enters the simulation
    if (has_component(entity, "Rigidbody2D"))
        wake_up(entity);
}

void detach_collider(Entity& entity, const std::string& collider_type) {
    if (!has_component(entity, collider_type)) return;
    entity["components"].erase(collider_type);
    if (has_component(entity, "Rigidbody2D"))
        wake_up(entity);
}

int get_attached_collider_count(const Entity& entity) {
    if (!entity.contains("components")) return 0;
    int count = 0;
    for (const char* t : COLLIDER_TYPES)
        if (has_component(entity, t)) ++count;
    return count;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊴  PHYSICS MATERIAL COMBINE MODES
// ════════════════════════════════════════════════════════════════════════════
// Maps the ext enum to the string the core solver already reads.
static const char* combine_str(CombineMode2D m) {
    switch (m) {
        case CombineMode2D::Average:  return "average";
        case CombineMode2D::Minimum:  return "minimum";
        case CombineMode2D::Multiply: return "multiply";
        case CombineMode2D::Maximum:  return "maximum";
    }
    return "average";
}

void set_friction_combine(Entity& entity, CombineMode2D mode, const std::string& col) {
    if (!has_component(entity, col)) return;
    entity["components"][col]["friction_combine"] = combine_str(mode);
}

void set_bounciness_combine(Entity& entity, CombineMode2D mode, const std::string& col) {
    if (!has_component(entity, col)) return;
    entity["components"][col]["bounciness_combine"] = combine_str(mode);
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊵  INERTIA TENSOR OVERRIDE
// ════════════════════════════════════════════════════════════════════════════
void set_inertia(Entity& entity, float inertia) {
    if (!has_component(entity, "Rigidbody2D")) return;
    entity["components"]["Rigidbody2D"]["inertia"] = std::max(1e-9f, finite_val(inertia));
}

float get_inertia(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return 0.f;
    float mass = std::max(1e-6f, (float)entity["components"]["Rigidbody2D"].value("mass", 1.f));
    return entity["components"]["Rigidbody2D"].value("inertia", mass * 100.f * 100.f / 6.f);
}

void reset_inertia(Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    if (rb.contains("inertia")) rb.erase("inertia");
    // Recompute via auto_compute_mass using existing mass
    float mass = rb.value("mass", 1.f);
    auto shapes = collect_shapes(entity);
    if (shapes.empty()) return;
    // Use the same inertia formula as physics.cpp: box = m(w²+h²)/12, circle = mr²/2
    // (auto_compute_mass recomputes both; we only want inertia here)
    auto [bx1, by1, bx2, by2] = shape_aabb(shapes[0]);
    float w = bx2 - bx1, h = by2 - by1;
    const auto& sh = shapes[0];
    float I = 0.f;
    if (sh.kind == ShapeKind::Circle)
        I = 0.5f * mass * sh.radius * sh.radius;
    else
        I = mass * (w * w + h * h) / 12.f;
    rb["inertia"] = std::max(1e-9f, I);
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊶  FORCE MODE VARIANTS
// ════════════════════════════════════════════════════════════════════════════
void add_force_mode(Entity& entity, float fx, float fy, ForceMode2D mode, float dt) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    float mass = std::max(finite_val(rb.value("mass", 1.f), 1.f), 1e-9f);

    switch (mode) {
        case ForceMode2D::Force:
            // Standard: accumulate force applied over the next dt
            add_force(rb, fx, fy);
            break;
        case ForceMode2D::Impulse:
            // Instant velocity change scaled by 1/mass
            add_impulse(rb, fx, fy);
            break;
        case ForceMode2D::VelocityChange:
            // Instant velocity change ignoring mass (like CharacterController)
            rb["velocity_x"] = finite_val((float)rb.value("velocity_x", 0.f) + fx);
            rb["velocity_y"] = finite_val((float)rb.value("velocity_y", 0.f) + fy);
            rb["_sleeping"] = false;
            break;
        case ForceMode2D::Acceleration:
            // Like Force but ignores mass: dv = a * dt
            add_force(rb, fx * mass, fy * mass);
            break;
    }
}

void add_force_at_mode(Entity& entity, float fx, float fy,
                        float wx, float wy, ForceMode2D mode, float dt) {
    if (!has_component(entity, "Rigidbody2D") || !has_component(entity, "Transform")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    auto& tr = entity["components"]["Transform"];
    float mass = std::max(finite_val(rb.value("mass", 1.f), 1.f), 1e-9f);
    float tx = get_float(tr, "x"), ty = get_float(tr, "y");
    float rx = wx - tx, ry = wy - ty;

    switch (mode) {
        case ForceMode2D::Force:
            add_force_at_point(rb, fx, fy, rx, ry);
            break;
        case ForceMode2D::Impulse:
            add_impulse_at_point(rb, fx, fy, rx, ry);
            break;
        case ForceMode2D::VelocityChange:
            add_force_at_point(rb, fx * mass, fy * mass, rx, ry);
            break;
        case ForceMode2D::Acceleration:
            add_force_at_point(rb, fx * mass, fy * mass, rx, ry);
            break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊷  COLLISION DETECTION MODE
// ════════════════════════════════════════════════════════════════════════════
void set_collision_detection_mode(Entity& entity, CollisionDetectionMode2D mode) {
    if (!has_component(entity, "Rigidbody2D")) return;
    auto& rb = entity["components"]["Rigidbody2D"];
    int mode_int = static_cast<int>(mode);
    rb["_collision_mode"] = mode_int;
    // Any Continuous mode enables the core CCD flag (apply_ccd in physics.cpp
    // keys off "continuous_collision" / "ccd").
    rb["continuous_collision"] = (mode != CollisionDetectionMode2D::Discrete);
    rb["ccd"] = rb["continuous_collision"];
}

CollisionDetectionMode2D get_collision_detection_mode(const Entity& entity) {
    if (!has_component(entity, "Rigidbody2D")) return CollisionDetectionMode2D::Discrete;
    const auto& rb = entity["components"]["Rigidbody2D"];
    if (rb.contains("_collision_mode")) {
        int m = rb.value("_collision_mode", 0);
        if (m >= 0 && m <= 3) return static_cast<CollisionDetectionMode2D>(m);
    }
    // Fallback: infer from existing CCD flag
    if (rb.value("continuous_collision", false) || rb.value("ccd", false))
        return CollisionDetectionMode2D::Continuous;
    return CollisionDetectionMode2D::Discrete;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊸  PHYSICS2D GLOBAL SETTINGS
// ════════════════════════════════════════════════════════════════════════════
static bool  s_queries_hit_triggers       = true;
static bool  s_queries_start_in_colliders = true;
static bool  s_auto_sync_transforms       = true;
static float s_max_linear_speed           = -1.f;   // <= 0 = disabled (use engine default)
static float s_max_angular_speed          = -1.f;   // <= 0 = disabled

void  set_queries_hit_triggers(bool v)       { s_queries_hit_triggers = v; }
bool  get_queries_hit_triggers()             { return s_queries_hit_triggers; }
void  set_queries_start_in_colliders(bool v) { s_queries_start_in_colliders = v; }
bool  get_queries_start_in_colliders()       { return s_queries_start_in_colliders; }
void  set_auto_sync_transforms(bool v)       { s_auto_sync_transforms = v; }
bool  get_auto_sync_transforms()             { return s_auto_sync_transforms; }
void  set_max_linear_speed(float pps) {
    s_max_linear_speed = pps;
    // Broadcast to all active bodies if we have a reference
    if (s_active_entities_ext) {
        for (auto& e : *s_active_entities_ext) {
            if (!has_component(e, "Rigidbody2D")) continue;
            e["components"]["Rigidbody2D"]["_max_linear_speed"] = pps;
        }
    }
}
float get_max_linear_speed() { return s_max_linear_speed; }
void  set_max_angular_speed(float dps) {
    s_max_angular_speed = dps;
    if (s_active_entities_ext) {
        for (auto& e : *s_active_entities_ext) {
            if (!has_component(e, "Rigidbody2D")) continue;
            e["components"]["Rigidbody2D"]["_max_angular_speed"] = dps;
        }
    }
}
float get_max_angular_speed() { return s_max_angular_speed; }

// ════════════════════════════════════════════════════════════════════════════
//  ㊹  CLAMP BODY SPEEDS
// ════════════════════════════════════════════════════════════════════════════
void clamp_body_speeds(EntityList& entities) {
    float max_lin = s_max_linear_speed;
    float max_ang = s_max_angular_speed;
    if (max_lin <= 0.f && max_ang <= 0.f) return; // nothing to clamp

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e, "Rigidbody2D")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;

        // Per-body override takes precedence
        float body_max_lin = rb.value("_max_linear_speed", max_lin);
        float body_max_ang = rb.value("_max_angular_speed", max_ang);

        if (body_max_lin > 0.f) {
            float vx = finite_val(rb.value("velocity_x", 0.f));
            float vy = finite_val(rb.value("velocity_y", 0.f));
            float spd = std::hypot(vx, vy);
            if (spd > body_max_lin && spd > 1e-12f) {
                float s = body_max_lin / spd;
                rb["velocity_x"] = vx * s;
                rb["velocity_y"] = vy * s;
            }
        }
        if (body_max_ang > 0.f) {
            float av = finite_val(rb.value("angular_velocity", 0.f)); // rad/s
            float max_rad = body_max_ang * (float)(M_PI / 180.0);
            if (std::abs(av) > max_rad) {
                rb["angular_velocity"] = (av > 0.f ? 1.f : -1.f) * max_rad;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊺  RIGIDBODY2D.SLIDE
// ════════════════════════════════════════════════════════════════════════════
//
// Helper: check if a normal represents a surface too steep to walk on.
// "up" direction is determined by surface_up_angle (default 90° = +Y in Nova coords).
static bool is_too_steep(float nx, float ny, float surface_up_deg, float max_slope_deg) {
    float up_rad   = surface_up_deg * (float)(M_PI / 180.0);
    float slope_rad = max_slope_deg * (float)(M_PI / 180.0);
    float ux = std::cos(up_rad), uy = std::sin(up_rad);
    float cos_angle = nx * ux + ny * uy; // dot with up
    // cos_angle > cos(slope_rad) means surface is within walkable slope
    return cos_angle < std::cos(slope_rad);
}

// Helper: project a displacement onto the tangent of a contact normal,
// discarding the component that pushes into the surface.
static Vec2 slide_along_surface(float dx, float dy, float nx, float ny) {
    float dot_into = dx * nx + dy * ny;
    if (dot_into < 0.f) {
        return {dx - dot_into * nx, dy - dot_into * ny};
    }
    return {dx, dy}; // already moving away from surface
}

SlideResults slide(Entity& entity, EntityList& world, Vec2 slide_move,
                   const SlideMovement& movement,
                   const std::vector<Entity*>& ignore_colliders)
{
    SlideResults results;
    if (!has_component(entity, "Rigidbody2D") || !has_component(entity, "Transform"))
        return results;

    auto& tr = entity["components"]["Transform"];
    float orig_x = get_float(tr, "x");
    float orig_y = get_float(tr, "y");
    int layer = 0;
    if (has_component(entity, "Rigidbody2D"))
        layer = entity["components"]["Rigidbody2D"].value("layer", 0);
    int layer_mask = (1 << layer);

    // Build ignore set
    std::unordered_set<Entity*> ignore_set;
    ignore_set.insert(&entity);
    for (auto* ig : ignore_colliders) ignore_set.insert(ig);

    // Fold gravity into slide_move
    float gx = movement.gravity.first;
    float gy = movement.gravity.second;
    float gmag = std::hypot(gx, gy);
    if (gmag > 1e-9f) {
        // Normalise and scale by magnitude * scale (unitless direction * user-provided magnitude)
        slide_move.first  += gx * movement.gravity_scale;
        slide_move.second += gy * movement.gravity_scale;
    }

    float move_dx = slide_move.first;
    float move_dy = slide_move.second;

    // ReuseCollision: slide using existing contacts from manifold cache
    if (movement.movement_type == SlideMovementType::ReuseCollision) {
        auto contacts = get_contacts(entity);
        float remaining_x = move_dx, remaining_y = move_dy;

        for (auto* cm : contacts) {
            if (!cm || cm->contact_count == 0) continue;
            float cnx = cm->nx, cny = cm->ny;
            // Ensure normal points away from the entity
            int eid = entity.value("id", 0);
            if (cm->e2 && cm->e2->value("id", 0) == eid) { cnx = -cnx; cny = -cny; }

            if (is_too_steep(cnx, cny, movement.surface_up_angle, movement.surface_angle)) {
                // Wall: zero out the movement component into the wall
                float dot_in = remaining_x * cnx + remaining_y * cny;
                if (dot_in < 0.f) {
                    remaining_x -= dot_in * cnx;
                    remaining_y -= dot_in * cny;
                }
                if (results.surface_hit_entity == nullptr) {
                    results.surface_hit_point = {cm->contacts[0].x, cm->contacts[0].y};
                    results.surface_hit_entity = cm->e2 ? cm->e2 : cm->e1;
                    results.surface_hit_collider = cm->col2 ? cm->col2 : cm->col1;
                }
            } else {
                // Walkable slope: slide along the surface tangent
                auto slid = slide_along_surface(remaining_x, remaining_y, cnx, cny);
                remaining_x = slid.first;
                remaining_y = slid.second;
                if (results.surface_hit_entity == nullptr) {
                    results.surface_hit_point = {cm->contacts[0].x, cm->contacts[0].y};
                    results.surface_hit_entity = cm->e2 ? cm->e2 : cm->e1;
                    results.surface_hit_collider = cm->col2 ? cm->col2 : cm->col1;
                }
            }
        }

        float final_x = orig_x + remaining_x;
        float final_y = orig_y + remaining_y;
        tr["x"] = final_x;
        tr["y"] = final_y;
        results.distance_moved = {final_x - orig_x, final_y - orig_y};
        transform::mark_local_dirty(entity.value("id", 0));
        return results;
    }

    // Linecast mode: sweep from current to target, stop at first contact,
    // then slide the remaining distance along the surface.
    if (movement.movement_type == SlideMovementType::Linecast) {
        float target_x = orig_x + move_dx;
        float target_y = orig_y + move_dy;
        float sweep_dist = std::hypot(move_dx, move_dy);
        if (sweep_dist < 1e-9f) return results;
        float snx = move_dx / sweep_dist, sny = move_dy / sweep_dist;

        // Build a query shape matching the entity's collider for the sweep
        auto shapes = collect_shapes(entity);
        if (shapes.empty()) return results;
        const auto& sh = shapes[0];

        std::optional<ShapeCastHit> first_hit;
        if (sh.kind == ShapeKind::Circle) {
            first_hit = circle_cast(world, orig_x, orig_y, sh.radius,
                                    snx, sny, sweep_dist, layer_mask,
                                    s_queries_hit_triggers);
        } else if (sh.kind == ShapeKind::Capsule) {
            first_hit = capsule_cast(world, orig_x, orig_y, sh.radius, sh.cap_half_h,
                                    sh.cap_horiz, snx, sny, sweep_dist, layer_mask,
                                    s_queries_hit_triggers);
        } else {
            auto [bx1, by1, bx2, by2] = shape_aabb(sh);
            first_hit = box_cast(world, orig_x, orig_y, bx2-bx1, by2-by1, 0.f,
                                 snx, sny, sweep_dist, layer_mask,
                                 s_queries_hit_triggers);
        }

        // Filter ignored entities
        while (first_hit && ignore_set.count(first_hit->entity)) {
            // Re-cast from beyond this hit
            float passed = first_hit->distance;
            float remain = sweep_dist - passed;
            if (remain < 1e-6f) { first_hit = std::nullopt; break; }
            float new_ox = orig_x + snx * (passed + 0.1f);
            float new_oy = orig_y + sny * (passed + 0.1f);
            if (sh.kind == ShapeKind::Circle) {
                first_hit = circle_cast(world, new_ox, new_oy, sh.radius,
                                        snx, sny, remain, layer_mask,
                                        s_queries_hit_triggers);
            } else if (sh.kind == ShapeKind::Capsule) {
                first_hit = capsule_cast(world, new_ox, new_oy, sh.radius, sh.cap_half_h,
                                        sh.cap_horiz, snx, sny, remain, layer_mask,
                                        s_queries_hit_triggers);
            } else {
                auto [bx1, by1, bx2, by2] = shape_aabb(sh);
                first_hit = box_cast(world, new_ox, new_oy, bx2-bx1, by2-by1, 0.f,
                                     snx, sny, remain, layer_mask,
                                     s_queries_hit_triggers);
            }
        }

        float actual_dist = sweep_dist;
        if (first_hit) {
            actual_dist = first_hit->distance;
            float hit_nx = first_hit->normal.first;
            float hit_ny = first_hit->normal.second;
            // Flip normal so it points away from the moving body
            // (cast normal points from query shape into hit; we want the surface normal
            // pointing outward from the surface toward the mover)
            // The cast normal already points "into" the hit surface from the mover's perspective
            // — we negate to get the surface outward normal
            hit_nx = -hit_nx;
            hit_ny = -hit_ny;

            results.surface_hit_point   = first_hit->point;
            results.surface_hit_entity  = first_hit->entity;
            results.surface_hit_collider = first_hit->collider ? first_hit->collider : first_hit->entity;

            float arrive_x = orig_x + snx * actual_dist;
            float arrive_y = orig_y + sny * actual_dist;

            // Slide remaining distance along the surface tangent
            float remain_x = move_dx - (arrive_x - orig_x);
            float remain_y = move_dy - (arrive_y - orig_y);

            if (is_too_steep(hit_nx, hit_ny, movement.surface_up_angle, movement.surface_angle)) {
                // Wall: absorb movement into the wall
                float dot_in = remain_x * hit_nx + remain_y * hit_ny;
                if (dot_in < 0.f) {
                    remain_x -= dot_in * hit_nx;
                    remain_y -= dot_in * hit_ny;
                }
            } else {
                auto slid = slide_along_surface(remain_x, remain_y, hit_nx, hit_ny);
                remain_x = slid.first;
                remain_y = slid.second;
            }

            float final_x = arrive_x + remain_x;
            float final_y = arrive_y + remain_y;
            tr["x"] = final_x;
            tr["y"] = final_y;
            results.distance_moved = {final_x - orig_x, final_y - orig_y};
        } else {
            // No hit: move full distance
            tr["x"] = target_x;
            tr["y"] = target_y;
            results.distance_moved = {move_dx, move_dy};
        }
        transform::mark_local_dirty(entity.value("id", 0));
        return results;
    }

    // ── MovePosition mode (default) ─────────────────────────────────────────
    // Move the body incrementally, checking for overlaps at each sub-step.
    constexpr int MAX_SLIDE_ITERS = 8;
    float remain_x = move_dx, remain_y = move_dy;
    float cur_x = orig_x, cur_y = orig_y;

    for (int iter = 0; iter < MAX_SLIDE_ITERS; ++iter) {
        float rem_mag = std::hypot(remain_x, remain_y);
        if (rem_mag < 0.01f) break;

        // Step size: move the full remainder but at most 1px per iteration
        // for stability (small steps catch edges better).
        float step_mag = std::min(rem_mag, 1.f);
        float snx = remain_x / rem_mag, sny = remain_y / rem_mag;
        float step_dx = snx * step_mag, step_dy = sny * step_mag;

        // Tentatively move
        float test_x = cur_x + step_dx;
        float test_y = cur_y + step_dy;
        tr["x"] = test_x;
        tr["y"] = test_y;

        // Collect the entity's shapes at the new position
        auto shapes = collect_shapes(entity);

        // Check for overlaps with world
        bool blocked = false;
        struct HitInfo {
            float nx, ny;
            float depth;
            Entity* hit_entity;
            Entity* hit_collider;
        };
        HitInfo best_hit = {0.f, 1.f, 0.f, nullptr, nullptr};

        for (const auto& sh : shapes) {
            for (auto& we : world) {
                if (!entity_active(we)) continue;
                if (ignore_set.count(&we)) continue;
                // Layer check
                if (has_component(we, "Rigidbody2D")) {
                    int wl = we["components"]["Rigidbody2D"].value("layer", 0);
                    if (!(layer_mask & (1 << wl))) continue;
                }
                if (!s_queries_hit_triggers && has_component(we, "Rigidbody2D")) {
                    // Check if any collider on this entity is a trigger
                    // (simplified: check the collider the shape is from)
                    if (sh.col && sh.col->value("is_trigger", false)) continue;
                }

                auto world_shapes = collect_shapes(we);
                for (auto& ws : world_shapes) {
                    if (!s_queries_hit_triggers && ws.col && ws.col->value("is_trigger", false))
                        continue;
                    Shape ms1 = sh, ms2 = ws;
                    auto m = dispatch_shapes(ms1, ms2);
                    if (!m || m->contact_count == 0) continue;

                    float hnx = m->nx, hny = m->ny;
                    float hdepth = m->contacts[0].depth;
                    Entity* hent = &we;
                    Entity* hcol = ws.col ? ws.col : &we;

                    // If normal points "into" our body (same direction as our movement),
                    // it's a blocking surface.
                    float move_dot = snx * hnx + sny * hny;
                    if (move_dot > 0.1f) {
                        // Normal is aligned with our movement — this surface blocks us.
                        // Flip normal to point away from mover.
                        hnx = -hnx; hny = -hny;
                        move_dot = -move_dot;
                    }
                    // Take the deepest blocking hit
                    if (hdepth > best_hit.depth) {
                        best_hit = {hnx, hny, hdepth, hent, hcol};
                    }
                    blocked = true;
                    break; // one hit per world entity per shape is enough
                }
                if (blocked) break;
            }
            if (blocked) break;
        }

        if (!blocked) {
            // Moved successfully — consume the step
            cur_x = test_x;
            cur_y = test_y;
            remain_x -= step_dx;
            remain_y -= step_dy;
        } else {
            // Revert the move and resolve the collision
            tr["x"] = cur_x;
            tr["y"] = cur_y;

            float hnx = best_hit.nx, hny = best_hit.ny;
            results.surface_hit_point   = {cur_x + hnx * best_hit.depth * 0.5f,
                                            cur_y + hny * best_hit.depth * 0.5f};
            results.surface_hit_entity  = best_hit.hit_entity;
            results.surface_hit_collider = best_hit.hit_collider;

            // Push body out along contact normal (minimum separation)
            cur_x += hnx * best_hit.depth;
            cur_y += hny * best_hit.depth;
            tr["x"] = cur_x;
            tr["y"] = cur_y;

            if (is_too_steep(hnx, hny, movement.surface_up_angle, movement.surface_angle)) {
                // Wall: remove the component of remaining movement into the wall
                float dot_in = remain_x * hnx + remain_y * hny;
                if (dot_in < 0.f) {
                    remain_x -= dot_in * hnx;
                    remain_y -= dot_in * hny;
                }
            } else {
                // Walkable surface: slide remaining movement along the tangent
                auto slid = slide_along_surface(remain_x, remain_y, hnx, hny);
                remain_x = slid.first;
                remain_y = slid.second;
            }
        }
    }

    results.distance_moved = {cur_x - orig_x, cur_y - orig_y};
    transform::mark_local_dirty(entity.value("id", 0));
    return results;
}

int slide_nonalloc(Entity& entity, EntityList& world, Vec2 slide_move,
                   SlideResults* results,
                   const SlideMovement& movement,
                   const std::vector<Entity*>& ignore_colliders)
{
    if (!results) return 0;
    *results = slide(entity, world, slide_move, movement, ignore_colliders);
    return 1;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊻  ANGULAR SPRING
// ════════════════════════════════════════════════════════════════════════════
void apply_angular_spring(Entity& entity, float target_deg,
                          float stiffness, float damping, float max_torque)
{
    if (!has_component(entity,"Rigidbody2D") || !has_component(entity,"Transform")) return;
    auto& rb  = entity["components"]["Rigidbody2D"];
    auto& tr  = entity["components"]["Transform"];
    if (body_type(rb) != "dynamic") return;

    float current_deg = finite_val(get_float(tr, "rotation"));
    // Angle error (shortest path)
    float err = target_deg - current_deg;
    while (err >  180.f) err -= 360.f;
    while (err < -180.f) err += 360.f;
    float err_rad = err * (float)M_PI / 180.f;

    float av = finite_val(rb.value("angular_velocity", 0.f));
    float torque = stiffness * err_rad - damping * av;
    torque = clamp(torque, -max_torque, max_torque);
    add_torque(rb, torque);
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊼  DRAG FIELD
// ════════════════════════════════════════════════════════════════════════════
void apply_drag_field(EntityList& entities, float cx, float cy, float radius,
                      float drag_coeff, float angular_drag)
{
    float r2 = radius * radius;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        if (rb.value("_sleeping", false)) continue;

        auto wt = transform::cached_world(e);
        float dx = finite_val(wt.x) - cx, dy = finite_val(wt.y) - cy;
        if (dx*dx + dy*dy > r2) continue;

        float mass = std::max(finite_val(rb.value("mass", 1.f), 1.f), 1e-9f);
        float vx = finite_val(rb.value("velocity_x", 0.f));
        float vy = finite_val(rb.value("velocity_y", 0.f));
        float spd = std::hypot(vx, vy);
        if (spd > 1e-6f) {
            // Quadratic drag: F = -coeff * spd² * direction
            float fd = -drag_coeff * spd * spd;
            float fdx = fd * (vx / spd), fdy = fd * (vy / spd);
            rb["_force_x"] = finite_val(rb.value("_force_x", 0.f)) + fdx * mass;
            rb["_force_y"] = finite_val(rb.value("_force_y", 0.f)) + fdy * mass;
        }
        // Angular drag in field
        float av = finite_val(rb.value("angular_velocity", 0.f));
        if (std::abs(av) > 1e-6f) {
            float I = std::max(finite_val(rb.value("inertia", mass * 100.f)), 1e-9f);
            add_torque(rb, -angular_drag * av * I);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊽  MAGNUS EFFECT
// ════════════════════════════════════════════════════════════════════════════
void set_magnus_enabled(bool enabled) { s_magnus_enabled = enabled; }

void apply_magnus_effect(EntityList& entities, float magnus_coeff)
{
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        if (rb.value("_sleeping", false)) continue;

        float av = finite_val(rb.value("angular_velocity", 0.f));
        if (std::abs(av) < 1e-6f) continue;

        float vx = finite_val(rb.value("velocity_x", 0.f));
        float vy = finite_val(rb.value("velocity_y", 0.f));
        float spd = std::hypot(vx, vy);
        if (spd < 1e-6f) continue;

        float mass = std::max(finite_val(rb.value("mass", 1.f), 1.f), 1e-9f);
        // In 2D: Magnus force = coeff * omega × v
        // omega in z × (vx,vy,0) = (-vy, vx, 0) * omega
        // Direction: ω × v where ω = av * ẑ, v = (vx, vy, 0)
        float lx = -vy * av * magnus_coeff * mass;
        float ly =  vx * av * magnus_coeff * mass;
        rb["_force_x"] = finite_val(rb.value("_force_x", 0.f)) + lx;
        rb["_force_y"] = finite_val(rb.value("_force_y", 0.f)) + ly;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊾  TRAJECTORY PREDICTION WITH DRAG
// ════════════════════════════════════════════════════════════════════════════
std::vector<TrajectoryPoint> predict_trajectory_with_drag(
    Vec2 start_pos, Vec2 start_vel,
    float gravity, float drag,
    int steps, float sub_dt)
{
    std::vector<TrajectoryPoint> pts;
    pts.reserve(steps);
    Vec2 gv = get_gravity_vector();
    float grav_x = gv.first  * (gravity / GRAVITY);  // scale to actual pixel gravity
    float grav_y = gv.second * (gravity / GRAVITY);

    float px = start_pos.first,  py = start_pos.second;
    float vx = start_vel.first,  vy = start_vel.second;
    float t  = 0.f;

    for (int i = 0; i < steps; ++i) {
        pts.push_back({{px, py}, {vx, vy}, t});

        // Exponential drag
        float damp = std::exp(-drag * sub_dt);
        float nvx = vx * damp + grav_x * sub_dt;
        float nvy = vy * damp + grav_y * sub_dt;
        px += nvx * sub_dt;
        py += nvy * sub_dt;
        vx = nvx; vy = nvy;
        t += sub_dt;
    }
    return pts;
}

// ════════════════════════════════════════════════════════════════════════════
//  ㊿  COYOTE-TIME GROUNDED QUERY
// ════════════════════════════════════════════════════════════════════════════
bool get_coyote_grounded(const Entity& entity, float coyote_window)
{
    if (!has_component(entity, "Rigidbody2D")) return false;
    auto& rb = entity["components"]["Rigidbody2D"];
    if (rb.value("_grounded", false)) return true;
    float window = coyote_window > 0.f ? coyote_window : COYOTE_TIME;
    return rb.value("_coyote_t", 9999.f) <= window;
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ①  VELOCITY DAMPING FIELD (wind / water resistance zone)
// ════════════════════════════════════════════════════════════════════════════
// More physically correct than area effectors for fluid simulation: applies
// Stokes drag (linear) or turbulent (quadratic) depending on body speed.
// Component JSON fields on entity with VelocityDampingField2D:
//   cx, cy, radius   — circle region
//   linear_coeff     — F = -linear_coeff * v  (low speed / viscous)
//   quadratic_coeff  — F = -quad_coeff * |v| * v  (high speed / turbulent)
//   flow_x, flow_y  — ambient flow velocity (subtracted before drag calc)
//   density         — fluid density scale (multiplies both coefficients)
void apply_velocity_damping_fields(EntityList& entities)
{
    struct Field {
        float cx, cy, r2;
        float lin, quad, density;
        float flow_x, flow_y;
    };
    std::vector<Field> fields;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e, "VelocityDampingField2D")) continue;
        auto& vdf = e["components"]["VelocityDampingField2D"];
        auto wt = transform::cached_world(e);
        Field f;
        f.cx  = finite_val(wt.x);
        f.cy  = finite_val(wt.y);
        float r = finite_val(vdf.value("radius", 100.f));
        f.r2  = r * r;
        f.lin = finite_val(vdf.value("linear_coeff", 0.f));
        f.quad= finite_val(vdf.value("quadratic_coeff", 0.002f));
        f.density = finite_val(vdf.value("density", 1.f), 1.f);
        f.flow_x= finite_val(vdf.value("flow_x", 0.f));
        f.flow_y= finite_val(vdf.value("flow_y", 0.f));
        fields.push_back(f);
    }
    if (fields.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic" || rb.value("_sleeping", false)) continue;
        auto wt = transform::cached_world(e);
        float ex = finite_val(wt.x), ey = finite_val(wt.y);
        for (auto& f : fields) {
            float dx = ex - f.cx, dy = ey - f.cy;
            if (dx*dx + dy*dy > f.r2) continue;
            float mass = std::max(finite_val(rb.value("mass",1.f), 1.f), 1e-9f);
            float vx = finite_val(rb.value("velocity_x",0.f)) - f.flow_x;
            float vy = finite_val(rb.value("velocity_y",0.f)) - f.flow_y;
            float spd = std::hypot(vx, vy);
            float fdx = 0.f, fdy = 0.f;
            if (spd > 1e-9f) {
                float mag = (f.lin + f.quad * spd) * f.density;
                fdx = -mag * vx; fdy = -mag * vy;
            }
            rb["_force_x"] = finite_val(rb.value("_force_x", 0.f)) + fdx * mass;
            rb["_force_y"] = finite_val(rb.value("_force_y", 0.f)) + fdy * mass;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ②  SOFT BODY 2D  (mass-spring network)
// ════════════════════════════════════════════════════════════════════════════
// A lightweight 2D soft body: a set of dynamic entities connected by springs.
// Each "link" connects two entity IDs with a rest length, stiffness, and damping.
// apply_soft_body_springs() processes all SoftBodyLink2D component entities.
//
// SoftBodyLink2D component JSON fields:
//   entity_a      (int)   — first particle entity id
//   entity_b      (int)   — second particle entity id
//   rest_length   (float) — equilibrium distance (0 = current distance)
//   stiffness     (float) — spring constant (default 300)
//   damping       (float) — velocity damping along spring axis (default 10)
//   break_length  (float) — distance at which spring breaks (0 = never)
void apply_soft_body_springs(EntityList& entities)
{
    // Build entity map
    std::unordered_map<int, Entity*> emap;
    for (auto& e : entities) emap[e.value("id", 0)] = &e;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e, "SoftBodyLink2D")) continue;
        auto& sbl = e["components"]["SoftBodyLink2D"];

        int id_a = sbl.value("entity_a", -1);
        int id_b = sbl.value("entity_b", -1);
        if (id_a < 0 || id_b < 0) continue;
        Entity* ea = emap.count(id_a) ? emap[id_a] : nullptr;
        Entity* eb = emap.count(id_b) ? emap[id_b] : nullptr;
        if (!ea || !eb) continue;
        if (!has_component(*ea,"Rigidbody2D")||!has_component(*ea,"Transform")) continue;
        if (!has_component(*eb,"Rigidbody2D")||!has_component(*eb,"Transform")) continue;

        auto& rba = (*ea)["components"]["Rigidbody2D"];
        auto& rbb = (*eb)["components"]["Rigidbody2D"];
        auto& ta  = (*ea)["components"]["Transform"];
        auto& tb  = (*eb)["components"]["Transform"];

        float ax = finite_val(get_float(ta,"x")), ay = finite_val(get_float(ta,"y"));
        float bx = finite_val(get_float(tb,"x")), by = finite_val(get_float(tb,"y"));
        float dx = bx - ax, dy = by - ay;
        float dist = std::hypot(dx, dy);
        if (dist < 1e-9f) continue;
        float nx = dx / dist, ny = dy / dist;

        float rest = sbl.value("rest_length", dist);
        // On first frame, cache rest length if 0 was specified
        if (rest <= 0.f) { sbl["rest_length"] = dist; rest = dist; }

        float stiffness = finite_val(sbl.value("stiffness", 300.f));
        float damp      = finite_val(sbl.value("damping",    10.f));

        // Break if exceeded
        float bl = finite_val(sbl.value("break_length", 0.f));
        if (bl > 0.f && dist > bl) {
            e["_broken"] = true;
            continue;
        }

        // Relative velocity along spring axis
        float vax = finite_val(rba.value("velocity_x",0.f));
        float vay = finite_val(rba.value("velocity_y",0.f));
        float vbx = finite_val(rbb.value("velocity_x",0.f));
        float vby = finite_val(rbb.value("velocity_y",0.f));
        float rv = dot(vbx - vax, vby - vay, nx, ny);

        float extension = dist - rest;
        float force_mag = stiffness * extension + damp * rv;
        float fx = force_mag * nx, fy = force_mag * ny;

        if (body_type(rba) == "dynamic") {
            rba["_force_x"] = finite_val(rba.value("_force_x",0.f)) + fx;
            rba["_force_y"] = finite_val(rba.value("_force_y",0.f)) + fy;
            rba["_sleeping"] = false;
        }
        if (body_type(rbb) == "dynamic") {
            rbb["_force_x"] = finite_val(rbb.value("_force_x",0.f)) - fx;
            rbb["_force_y"] = finite_val(rbb.value("_force_y",0.f)) - fy;
            rbb["_sleeping"] = false;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ③  POSITION-BASED VELOCITY CORRECTION (sub-step smoothing)
// ════════════════════════════════════════════════════════════════════════════
// After integration, if two dynamic bodies overlap beyond a threshold, apply
// a small velocity correction to push them apart without energy injection.
// This is a complement to split-impulse position correction — it catches cases
// where the solver missed a contact due to large sub-steps or CCD failures.
// Call AFTER apply_physics_ext() if you want an extra stabilisation pass.
void apply_depenetration_pass(EntityList& entities, float dt, float max_correction)
{
    std::unordered_map<int, Entity*> emap;
    for (auto& e : entities) emap[e.value("id",0)] = &e;

    for (auto& e1 : entities) {
        if (!entity_active(e1) || !has_component(e1,"Rigidbody2D") || !has_component(e1,"Transform")) continue;
        auto& rb1 = e1["components"]["Rigidbody2D"];
        if (body_type(rb1) != "dynamic") continue;
        if (rb1.value("_sleeping",false)) continue;

        auto shapes1 = collect_shapes(e1);
        for (auto& e2 : entities) {
            if (&e1 == &e2 || !entity_active(e2)) continue;
            if (!has_component(e2,"Rigidbody2D") || !has_component(e2,"Transform")) continue;
            auto& rb2 = e2["components"]["Rigidbody2D"];
            if (body_type(rb2) == "static" && !rb2.value("_sleeping",false)) {
                /* static vs dynamic is handled by normal narrow phase */
            }
            if (body_type(rb2) != "dynamic") continue;

            auto shapes2 = collect_shapes(e2);
            for (auto& s1 : shapes1) {
                for (auto& s2 : shapes2) {
                    // Quick AABB pre-check
                    auto [ax1,ay1,ax2,ay2] = shape_aabb(s1);
                    auto [bx1,by1,bx2,by2] = shape_aabb(s2);
                    if (ax2 < bx1 || ax1 > bx2 || ay2 < by1 || ay1 > by2) continue;

                    auto ms1 = s1, ms2 = s2;
                    auto mfd = dispatch_shapes(ms1, ms2);
                    if (!mfd || mfd->contact_count == 0) continue;

                    // Only act on significant overlap not handled by solver
                    for (int ci = 0; ci < mfd->contact_count; ++ci) {
                        float pen = mfd->contacts[ci].depth;
                        if (pen < SLOP * 3.f) continue;
                        // Gentle position correction velocity (no restitution)
                        float corr = std::min(pen * 0.5f / std::max(dt, 1e-6f), max_correction);
                        float inv_m1 = 1.f / std::max(finite_val(rb1.value("mass",1.f)), 1e-9f);
                        float inv_m2 = 1.f / std::max(finite_val(rb2.value("mass",1.f)), 1e-9f);
                        float total  = inv_m1 + inv_m2;
                        if (total < 1e-12f) continue;
                        float c1 = corr * inv_m1 / total;
                        float c2 = corr * inv_m2 / total;
                        rb1["velocity_x"] = finite_val(rb1.value("velocity_x",0.f)) - mfd->nx * c1;
                        rb1["velocity_y"] = finite_val(rb1.value("velocity_y",0.f)) - mfd->ny * c1;
                        rb2["velocity_x"] = finite_val(rb2.value("velocity_x",0.f)) + mfd->nx * c2;
                        rb2["velocity_y"] = finite_val(rb2.value("velocity_y",0.f)) + mfd->ny * c2;
                    }
                }
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ④  CONTACT RESTITUTION SMOOTHING (velocity threshold blend)
// ════════════════════════════════════════════════════════════════════════════
// Smoothly blends restitution to zero near the threshold velocity, instead
// of the hard step used in the core solver. This eliminates the micro-bounce
// artifact where objects barely above threshold bounce unexpectedly.
// Call once at setup to override the global threshold and enable smooth blend.
static float s_restitution_blend_range = 20.f;  // blend range in pixels/s
static bool  s_restitution_blend_enabled = false;

void set_restitution_blend(bool enabled, float blend_range) {
    s_restitution_blend_enabled = enabled;
    s_restitution_blend_range   = std::max(1.f, blend_range);
}
bool get_restitution_blend() { return s_restitution_blend_enabled; }

// Compute effective restitution for a given relative normal velocity.
// Uses a smooth quintic falloff in the threshold band, zero below.
float compute_smooth_restitution(float raw_restitution, float rel_normal_vel) {
    if (!s_restitution_blend_enabled) return raw_restitution;
    float v = std::abs(rel_normal_vel);
    float lo = RESTITUTION_THRESH;
    float hi = lo + s_restitution_blend_range;
    if (v <= lo) return 0.f;
    if (v >= hi) return raw_restitution;
    float t = (v - lo) / (hi - lo);
    // Quintic smoothstep: 6t⁵ - 15t⁴ + 10t³
    float s = t * t * t * (t * (t * 6.f - 15.f) + 10.f);
    return raw_restitution * s;
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ⑤  TERRAIN NORMAL SMOOTHING  (slope interpolation)
// ════════════════════════════════════════════════════════════════════════════
// For tilemaps and EdgeCollider2D chains, contacts at tile corners produce
// jitter due to normal discontinuities. This pass blends the contact normals
// of neighbouring contacts toward their average, producing a smooth slope feel.
// Should be called once per frame BEFORE solve_velocities() inside the substep
// (this is done automatically inside apply_physics_ext).
void smooth_contact_normals(std::vector<Manifold>& manifolds)
{
    // Group contacts by entity pair, then blend normals within the group
    // using a distance-weighted average — contacts close to a tile edge
    // get a blend of the two adjacent face normals.
    struct Group {
        std::vector<Manifold*> mfds;
        float avg_nx = 0.f, avg_ny = 0.f;
    };
    std::unordered_map<long long, Group> groups;

    for (auto& m : manifolds) {
        if (m.is_trigger || m.one_way_skip || m.contact_count == 0) continue;
        int id1 = m.e1 ? m.e1->value("id",0) : -1;
        int id2 = m.e2 ? m.e2->value("id",0) : -1;
        if (id1 < 0 || id2 < 0) continue;
        // Same entity pair — group regardless of sub-shape
        long long key = (long long)std::min(id1,id2) << 32 | (unsigned)std::max(id1,id2);
        groups[key].mfds.push_back(&m);
        groups[key].avg_nx += m.nx;
        groups[key].avg_ny += m.ny;
    }

    for (auto& [key, g] : groups) {
        if (g.mfds.size() < 2) continue;
        float cnt = (float)g.mfds.size();
        float anx = g.avg_nx / cnt, any = g.avg_ny / cnt;
        float alen = std::hypot(anx, any);
        if (alen < 1e-9f) continue;
        anx /= alen; any /= alen;
        // Light blend toward average normal — keeps individual face response
        // but removes corner discontinuities
        constexpr float BLEND = 0.25f;
        for (auto* mp : g.mfds) {
            float dot_with_avg = mp->nx * anx + mp->ny * any;
            if (dot_with_avg < 0.3f) continue;  // opposite half-space — skip
            mp->nx = mp->nx * (1.f - BLEND) + anx * BLEND;
            mp->ny = mp->ny * (1.f - BLEND) + any * BLEND;
            float nl = std::hypot(mp->nx, mp->ny);
            if (nl > 1e-9f) { mp->nx /= nl; mp->ny /= nl; }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ⑥  PER-BODY AIR RESISTANCE  (shape-aware drag)
// ════════════════════════════════════════════════════════════════════════════
// Applies drag proportional to the body's cross-sectional area (estimated
// from its collider bounds). Larger / flatter objects slow faster. This is
// much more realistic than Unity's uniform drag scalar.
// Enable via rb["shape_drag"] = true (default: false, uses normal rb.drag).
// drag_coeff   — base drag coefficient (Cd, default 0.47 for sphere/circle)
// air_density  — fluid density (default 0.001225 for air at sea level in px units)
void apply_shape_aware_drag(EntityList& entities, float air_density)
{
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        if (!rb.value("shape_drag", false)) continue;
        if (rb.value("_sleeping", false)) continue;

        float vx = finite_val(rb.value("velocity_x",0.f));
        float vy = finite_val(rb.value("velocity_y",0.f));
        float spd = std::hypot(vx, vy);
        if (spd < 1e-6f) continue;

        // Estimate cross-sectional area from AABB
        auto shapes = collect_shapes(e);
        if (shapes.empty()) continue;
        auto [sx1,sy1,sx2,sy2] = shape_aabb(shapes[0]);
        float width  = sx2 - sx1;
        float height = sy2 - sy1;
        // Project cross-section perpendicular to velocity
        float vnx = vx / spd, vny = vy / spd;
        // Cross-section = w * |sin(v_angle)| + h * |cos(v_angle)|
        float area = width * std::abs(vny) + height * std::abs(vnx);
        area = std::max(area, 1.f);  // at least 1px²

        float cd = finite_val(rb.value("drag_coeff", 0.47f));
        float mass = std::max(finite_val(rb.value("mass",1.f)), 1e-9f);
        // F_drag = -½ ρ Cd A v²  (direction opposite to motion)
        float F = 0.5f * air_density * cd * area * spd * spd;
        // Cap to prevent reversal
        F = std::min(F, mass * spd / (1.f/60.f));
        rb["_force_x"] = finite_val(rb.value("_force_x",0.f)) - F * vnx;
        rb["_force_y"] = finite_val(rb.value("_force_y",0.f)) - F * vny;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ⑦  VERLET INTEGRATION MODE  (alternative integrator)
// ════════════════════════════════════════════════════════════════════════════
// Velocity Verlet gives better energy conservation than semi-implicit Euler
// for spring systems and soft bodies. Enable per-body with rb["use_verlet"]=true.
// Stores previous position in rb["_prev_x"/"_prev_y"] automatically.
// NOTE: this integrator runs AFTER apply_physics_ext() in a post-pass; joints
// and contacts from the normal solver still apply beforehand.
void apply_verlet_bodies(EntityList& entities, float dt, float gravity)
{
    Vec2 gv = get_gravity_vector();
    float grav_x = gv.first  * (gravity / GRAVITY);
    float grav_y = gv.second * (gravity / GRAVITY);

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        if (!rb.value("use_verlet", false)) continue;
        if (rb.value("_sleeping", false)) continue;

        auto& tr = e["components"]["Transform"];
        float x  = finite_val(get_float(tr, "x"));
        float y  = finite_val(get_float(tr, "y"));
        float px = finite_val(rb.value("_prev_x", x));
        float py = finite_val(rb.value("_prev_y", y));
        float mass = std::max(finite_val(rb.value("mass",1.f)), 1e-9f);

        float fx = finite_val(rb.value("_force_x",0.f));
        float fy = finite_val(rb.value("_force_y",0.f));
        float gs = finite_val(rb.value("gravity_scale",1.f), 1.f);
        float ax = grav_x * gs + fx / mass;
        float ay = grav_y * gs + fy / mass;

        // Drag: reduce displacement by drag factor
        float drag = std::max(0.f, finite_val(rb.value("drag", 0.05f)));
        float damp = std::exp(-drag * dt);

        // Verlet: x_new = 2*x - x_prev + a*dt²   (with damping)
        float nx_pos = x + (x - px) * damp + ax * dt * dt;
        float ny_pos = y + (y - py) * damp + ay * dt * dt;

        // Update velocity from position difference (used by collision solver)
        rb["velocity_x"] = (nx_pos - x) / dt;
        rb["velocity_y"] = (ny_pos - y) / dt;

        // Store previous and advance
        rb["_prev_x"] = x;
        rb["_prev_y"] = y;
        tr["x"] = nx_pos;
        tr["y"] = ny_pos;
        transform::mark_local_dirty(e.value("id",0));

        // Clear forces (already applied)
        rb.erase("_force_x"); rb.erase("_force_y");
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ⑧  GRAVITY WELL  (planetary / vortex gravity)
// ════════════════════════════════════════════════════════════════════════════
// A gravity well attracts (or repels) bodies toward a point with an
// inverse-square fall-off — matching planetary/star gravity.
// Component JSON fields on entity with GravityWell2D:
//   mass        (float) — gravitational mass (scales attraction force)
//   G           (float) — gravitational constant (default 6.674e-11 * pixel_scale)
//   min_radius  (float) — clamp distance to avoid singularity
//   max_radius  (float) — beyond this, no effect (default 0 = infinite)
//   repulsive   (bool)  — if true, pushes away instead of attracting
void apply_gravity_wells(EntityList& entities)
{
    struct Well {
        float wx, wy, G_mass, min_r2, max_r2;
        bool repulsive;
    };
    std::vector<Well> wells;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e, "GravityWell2D")) continue;
        auto& gw = e["components"]["GravityWell2D"];
        auto wt = transform::cached_world(e);
        Well w;
        w.wx = finite_val(wt.x); w.wy = finite_val(wt.y);
        float G  = finite_val(gw.value("G", 500.f));         // default pixel-scaled G
        float m  = finite_val(gw.value("mass", 1000.f));
        w.G_mass = G * m;
        float minr = finite_val(gw.value("min_radius", 10.f));
        w.min_r2 = minr * minr;
        float maxr = finite_val(gw.value("max_radius", 0.f));
        w.max_r2 = (maxr > 0.f) ? maxr * maxr : 1e30f;
        w.repulsive = gw.value("repulsive", false);
        wells.push_back(w);
    }
    if (wells.empty()) return;

    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
        auto& rb = e["components"]["Rigidbody2D"];
        if (body_type(rb) != "dynamic") continue;
        if (rb.value("_sleeping", false)) continue;
        float mass = std::max(finite_val(rb.value("mass",1.f)), 1e-9f);
        auto wt = transform::cached_world(e);
        float ex = finite_val(wt.x), ey = finite_val(wt.y);

        for (auto& w : wells) {
            float dx = w.wx - ex, dy = w.wy - ey;
            float d2 = dx*dx + dy*dy;
            if (d2 < w.min_r2) d2 = w.min_r2;
            if (d2 > w.max_r2) continue;
            float d = std::sqrt(d2);
            // F = G*M*m / r²  (direction toward well)
            float F = w.G_mass * mass / d2;
            if (w.repulsive) F = -F;
            float fx = F * dx / d, fy = F * dy / d;
            rb["_force_x"] = finite_val(rb.value("_force_x",0.f)) + fx;
            rb["_force_y"] = finite_val(rb.value("_force_y",0.f)) + fy;
            rb["_sleeping"] = false;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ⑨  IMPACT SOUND EVENTS  (velocity-based collision audio hook)
// ════════════════════════════════════════════════════════════════════════════
// Fires an event when two bodies collide with a relative normal velocity
// above a threshold. Useful for triggering collision sounds with realistic
// volume scaling (louder = harder impact).
//
// Usage:
//   phys::set_impact_event_listener([](Entity* a, Entity* b, float impact_vel) {
//       float volume = std::min(impact_vel / 500.f, 1.f);
//       audio::play("impact", volume);
//   });
static std::function<void(Entity*, Entity*, float /*impact_vel*/)> s_impact_listener;

void set_impact_event_listener(std::function<void(Entity*, Entity*, float)> cb) {
    s_impact_listener = std::move(cb);
}

void dispatch_impact_events(const std::vector<Manifold>& manifolds, float threshold)
{
    if (!s_impact_listener) return;
    for (auto& m : manifolds) {
        if (m.is_trigger || m.one_way_skip || m.contact_count == 0) continue;
        // Relative normal velocity = sum of normal impulses / dt (approx)
        // Use lambda_n as a proxy for impact strength
        float total_impulse = 0.f;
        for (int ci = 0; ci < m.contact_count; ++ci)
            total_impulse += m.contacts[ci].lambda_n;
        if (total_impulse < threshold) continue;
        s_impact_listener(m.e1, m.e2, total_impulse);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  NEW FEATURE ⑩  EXPLOSION RING  (expanding shockwave)
// ════════════════════════════════════════════════════════════════════════════
// Simulates a ring-shaped explosion shockwave that expands outward at a
// given speed. Bodies are pushed when the ring passes through them.
// Unlike explosion_force which is instant, this tracks a live ring.
//
// Register a ring and call update_explosion_rings() each frame.
struct ExplosionRing {
    float cx, cy;
    float current_radius;
    float expand_speed;     // pixels/s
    float ring_width;       // thickness of the shockwave ring
    float peak_force;       // force at peak (falls off across ring width)
    float lifetime;         // remaining seconds
    int   layer_mask;
    bool  alive = true;
};

static std::vector<ExplosionRing> s_explosion_rings;

void add_explosion_ring(float cx, float cy, float expand_speed, float peak_force,
                        float lifetime, float ring_width, int layer_mask)
{
    ExplosionRing r;
    r.cx = cx; r.cy = cy;
    r.current_radius = 0.f;
    r.expand_speed   = expand_speed;
    r.ring_width     = ring_width > 0.f ? ring_width : expand_speed * 0.1f;
    r.peak_force     = peak_force;
    r.lifetime       = lifetime;
    r.layer_mask     = layer_mask;
    r.alive          = true;
    s_explosion_rings.push_back(r);
}

void update_explosion_rings(EntityList& entities, float dt)
{
    for (auto& ring : s_explosion_rings) {
        if (!ring.alive) continue;
        ring.current_radius += ring.expand_speed * dt;
        ring.lifetime -= dt;
        if (ring.lifetime <= 0.f) { ring.alive = false; continue; }

        float inner = ring.current_radius - ring.ring_width * 0.5f;
        float outer = ring.current_radius + ring.ring_width * 0.5f;
        float inner2 = inner * inner, outer2 = outer * outer;

        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Rigidbody2D") || !has_component(e,"Transform")) continue;
            auto& rb = e["components"]["Rigidbody2D"];
            if (body_type(rb) != "dynamic") continue;
            auto wt = transform::cached_world(e);
            float dx = finite_val(wt.x) - ring.cx, dy = finite_val(wt.y) - ring.cy;
            float d2 = dx*dx + dy*dy;
            if (d2 < inner2 || d2 > outer2) continue;
            float d = std::sqrt(d2);
            if (d < 1e-9f) continue;
            // Smooth falloff within ring width (Gaussian-ish)
            float t = (d - ring.current_radius) / (ring.ring_width * 0.5f);
            float falloff = std::exp(-3.f * t * t);  // peak at ring edge
            float fx = (dx/d) * ring.peak_force * falloff;
            float fy = (dy/d) * ring.peak_force * falloff;
            add_impulse(rb, fx * dt, fy * dt);
        }
    }
    // Prune dead rings
    s_explosion_rings.erase(
        std::remove_if(s_explosion_rings.begin(), s_explosion_rings.end(),
                       [](const ExplosionRing& r){ return !r.alive; }),
        s_explosion_rings.end());
}

void clear_explosion_rings() { s_explosion_rings.clear(); }
int  get_explosion_ring_count() { return (int)s_explosion_rings.size(); }

// ════════════════════════════════════════════════════════════════════════════
//  RUNTIME PHYSICS SETTINGS  (Physics2D global tunable equivalents)
//  Exposes Baumgarte scale, TOI Baumgarte, and defaultContactOffset (SLOP)
//  as runtime-settable globals, matching Unity's Physics2D settings API.
// ════════════════════════════════════════════════════════════════════════════
static float s_baumgarte_scale       = BAUMGARTE;   // default from compile-time constant
static float s_baumgarte_toi_scale   = BAUMGARTE * 0.75f; // Unity uses a lower value for TOI
static float s_default_contact_offset = SLOP;        // Unity's defaultContactOffset ≈ SLOP

void  set_baumgarte_scale(float v)          { s_baumgarte_scale       = std::max(0.f, v); }
float get_baumgarte_scale()                 { return s_baumgarte_scale; }
void  set_baumgarte_toi_scale(float v)      { s_baumgarte_toi_scale   = std::max(0.f, v); }
float get_baumgarte_toi_scale()             { return s_baumgarte_toi_scale; }
void  set_default_contact_offset(float v)   { s_default_contact_offset = std::max(0.f, v); }
float get_default_contact_offset()          { return s_default_contact_offset; }

// ════════════════════════════════════════════════════════════════════════════
//  NAMED LAYER REGISTRY  (LayerMask.NameToLayer / LayerToName equivalent)
//  Maps string layer names to integer layer indices [0..31].
//  set_layer_name(3, "Ground")  →  layer_name_to_index("Ground") == 3
// ════════════════════════════════════════════════════════════════════════════
static std::unordered_map<std::string,int> s_layer_name_to_idx;
static std::unordered_map<int,std::string> s_layer_idx_to_name;

void set_layer_name(int layer_index, const std::string& name) {
    if (layer_index < 0 || layer_index > 31) return;
    // Remove old reverse mapping if any
    if (s_layer_idx_to_name.count(layer_index))
        s_layer_name_to_idx.erase(s_layer_idx_to_name[layer_index]);
    s_layer_idx_to_name[layer_index] = name;
    s_layer_name_to_idx[name] = layer_index;
}

int layer_name_to_index(const std::string& name) {
    auto it = s_layer_name_to_idx.find(name);
    return (it != s_layer_name_to_idx.end()) ? it->second : -1;
}

std::string layer_index_to_name(int layer_index) {
    auto it = s_layer_idx_to_name.find(layer_index);
    return (it != s_layer_idx_to_name.end()) ? it->second : "";
}

// LayerMask from names — OR together the bit for each named layer.
// Usage: int mask = layer_mask_from_names({"Ground","Platform"});
int layer_mask_from_names(const std::vector<std::string>& names) {
    int mask = 0;
    for (auto& n : names) {
        int idx = layer_name_to_index(n);
        if (idx >= 0) mask |= (1 << idx);
    }
    return mask;
}

// ════════════════════════════════════════════════════════════════════════════
//  SHARED PHYSICSMATERIAL2D  (shared material references)
//  Materials are keyed by name. Changing a material via set_shared_material()
//  updates all colliders that reference it on the next collect_shapes() pass
//  by re-reading from the registry at runtime rather than copy-on-build.
// ════════════════════════════════════════════════════════════════════════════

static std::unordered_map<std::string, PhysicsMaterial2D> s_shared_materials;

void register_physics_material(const std::string& name, const PhysicsMaterial2D& mat) {
    s_shared_materials[name] = mat;
}
bool get_physics_material(const std::string& name, PhysicsMaterial2D& out) {
    auto it = s_shared_materials.find(name);
    if (it == s_shared_materials.end()) return false;
    out = it->second;
    return true;
}
void update_physics_material(const std::string& name, const PhysicsMaterial2D& mat) {
    s_shared_materials[name] = mat;
}

// Apply a shared material to all colliders in the entity list that reference it.
// Call after update_physics_material() to propagate the change immediately.
void sync_shared_material(EntityList& entities, const std::string& name) {
    auto it = s_shared_materials.find(name);
    if (it == s_shared_materials.end()) return;
    const auto& mat = it->second;
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        for (auto comp_name : {"BoxCollider2D","CircleCollider2D","PolygonCollider2D",
                               "CapsuleCollider2D","EdgeCollider2D"}) {
            if (!has_component(e, comp_name)) continue;
            auto& col = e["components"][comp_name];
            if (col.value("material", std::string("")) != name) continue;
            col["friction"]   = mat.friction;
            col["bounciness"] = mat.bounciness;
            col["static_friction"]  = mat.static_friction;
            col["kinetic_friction"] = mat.kinetic_friction;
            col["friction_combine"] = mat.friction_combine;
            col["bounce_combine"]   = mat.bounce_combine;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  overlap_capsule() — plain variant (no contact info, matches Unity API)
//  overlap_capsule_ex() already exists with full contact data; this is the
//  simple variant that returns a list of hit entities, matching Unity's
//  Physics2D.OverlapCapsule() signature.
// ════════════════════════════════════════════════════════════════════════════
std::vector<Entity*> overlap_capsule(EntityList& entities,
                                     float cx, float cy,
                                     float radius, float half_height,
                                     bool horizontal,
                                     int layer_mask, bool query_triggers)
{
    auto ext_hits = overlap_capsule_ex(entities, cx, cy, radius, half_height,
                                       horizontal, ContactFilter2D{false,0,0,false,0,360,true,layer_mask,query_triggers});
    std::vector<Entity*> result;
    result.reserve(ext_hits.size());
    for (auto& h : ext_hits) result.push_back(h.entity);
    return result;
}

// ════════════════════════════════════════════════════════════════════════════
//  OverlapArea(pointA, pointB) — Unity Physics2D.OverlapArea convenience
//  Returns entities overlapping the axis-aligned box defined by two corners.
// ════════════════════════════════════════════════════════════════════════════
std::vector<Entity*> overlap_area(EntityList& entities,
                                  float ax, float ay, float bx, float by,
                                  int layer_mask, bool query_triggers)
{
    float x  = (ax + bx) * 0.5f;
    float y  = (ay + by) * 0.5f;
    float w  = std::abs(bx - ax);
    float h  = std::abs(by - ay);
    return overlap_box(entities, x, y, w, h, 0.f, layer_mask);
}

std::vector<Entity*> overlap_area_all(EntityList& entities,
                                      float ax, float ay, float bx, float by,
                                      int layer_mask, bool query_triggers)
{
    return overlap_area(entities, ax, ay, bx, by, layer_mask, query_triggers);
}

// ════════════════════════════════════════════════════════════════════════════
//  GetAttachedColliders() — iterate colliders on an entity
//  Returns pointers to each collider component JSON node on the entity.
//  Matches Unity's Rigidbody2D.GetAttachedColliders(results) API.
// ════════════════════════════════════════════════════════════════════════════
std::vector<Entity*> get_attached_colliders(Entity& entity) {
    std::vector<Entity*> result;
    static const char* COLLIDER_TYPES[] = {
        "BoxCollider2D","CircleCollider2D","PolygonCollider2D",
        "CapsuleCollider2D","EdgeCollider2D","CompositeCollider2D",
        "TilemapCollider2D", nullptr
    };
    for (int i = 0; COLLIDER_TYPES[i]; ++i) {
        if (has_component(entity, COLLIDER_TYPES[i]))
            result.push_back(&entity["components"][COLLIDER_TYPES[i]]);
    }
    return result;
}

int get_attached_collider_count(Entity& entity) {
    int count = 0;
    static const char* COLLIDER_TYPES[] = {
        "BoxCollider2D","CircleCollider2D","PolygonCollider2D",
        "CapsuleCollider2D","EdgeCollider2D","CompositeCollider2D",
        "TilemapCollider2D", nullptr
    };
    for (int i = 0; COLLIDER_TYPES[i]; ++i)
        if (has_component(entity, COLLIDER_TYPES[i])) ++count;
    return count;
}

// ════════════════════════════════════════════════════════════════════════════
//  CustomCollider2D — runtime convex polygon array per body
//  Unity 2022+ API: allows arbitrary arrays of convex polygons per body
//  with runtime modification (SetCustomShapes / ClearCustomShapes).
//  Stored in entity["components"]["CustomCollider2D"]["shapes"] as a JSON
//  array of polygon vertex lists.
// ════════════════════════════════════════════════════════════════════════════
void custom_collider_set_shapes(Entity& entity,
                                const std::vector<std::vector<std::pair<float,float>>>& polygons)
{
    if (!entity["components"].contains("CustomCollider2D"))
        entity["components"]["CustomCollider2D"] = Entity::object();
    auto& cc = entity["components"]["CustomCollider2D"];
    cc["shapes"] = Entity::array();
    for (auto& poly : polygons) {
        Entity poly_arr = Entity::array();
        for (auto& [px, py] : poly) {
            Entity pt = Entity::object();
            pt["x"] = px; pt["y"] = py;
            poly_arr.push_back(pt);
        }
        cc["shapes"].push_back(poly_arr);
    }
    cc["enabled"] = true;
}

void custom_collider_clear_shapes(Entity& entity) {
    if (has_component(entity, "CustomCollider2D"))
        entity["components"]["CustomCollider2D"]["shapes"] = Entity::array();
}

// ════════════════════════════════════════════════════════════════════════════
//  autoConfigureConnectedAnchor — set connected anchor automatically
//  When creating a joint at runtime, call this to set connected_anchor_x/y
//  on the joint component to match the current world-space anchor, mirroring
//  Unity's AnchoredJoint2D.autoConfigureConnectedAnchor behaviour.
// ════════════════════════════════════════════════════════════════════════════
void auto_configure_connected_anchor(Entity& entity, const std::string& joint_type) {
    if (!has_component(entity, joint_type)) return;
    if (!has_component(entity, "Transform")) return;
    auto& jc = entity["components"][joint_type];
    auto& t1 = entity["components"]["Transform"];

    float ax = finite_val(jc.value("anchor_x", 0.f));
    float ay = finite_val(jc.value("anchor_y", 0.f));
    float rot = finite_val(get_float(t1, "rotation")) * (float)M_PI / 180.f;
    float c = std::cos(rot), s = std::sin(rot);
    float world_ax = finite_val(get_float(t1, "x")) + ax * c - ay * s;
    float world_ay = finite_val(get_float(t1, "y")) + ax * s + ay * c;

    jc["connected_anchor_x"] = world_ax;
    jc["connected_anchor_y"] = world_ay;
}

// ════════════════════════════════════════════════════════════════════════════
//  WheelJoint2D — lateral friction constraint (cornering)
//  Applies an impulse constraining sideways slip velocity to zero,
//  matching Unity WheelJoint2D's lateral friction behaviour.
//  Call after apply_physics_ext() or integrate into solve_joints().
// ════════════════════════════════════════════════════════════════════════════
void apply_wheel_lateral_friction(EntityList& entities, float dt,
                                   std::unordered_map<int,Entity*>& emap)
{
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        if (!has_component(e, "WheelJoint2D")) continue;
        if (!has_component(e, "Rigidbody2D"))  continue;
        if (!has_component(e, "Transform"))    continue;
        auto& wj = e["components"]["WheelJoint2D"];
        if (!wj.value("use_lateral_friction", true)) continue;
        auto& rb1 = e["components"]["Rigidbody2D"];
        auto& t1  = e["components"]["Transform"];
        if (is_static(&rb1)) continue;

        float max_lateral = finite_val(wj.value("max_lateral_impulse", 1e4f));
        float im1 = body_inv_mass(&rb1);
        if (im1 <= 0) continue;

        // The wheel's lateral axis is perpendicular to the suspension axis.
        // Suspension axis is along the link to the connected entity; default up (0,-1).
        int oid = wj.value("connected_entity", -1);
        float lat_nx = 1.f, lat_ny = 0.f; // default: constrain X slip
        if (emap.count(oid)) {
            auto* other = emap[oid];
            if (has_component(*other, "Transform")) {
                auto& t2 = (*other)["components"]["Transform"];
                float dx = finite_val(get_float(t2,"x")) - finite_val(get_float(t1,"x"));
                float dy = finite_val(get_float(t2,"y")) - finite_val(get_float(t1,"y"));
                float len = std::hypot(dx, dy);
                if (len > 1e-6f) {
                    // Suspension direction: body to connected body
                    float sx = dx/len, sy = dy/len;
                    // Lateral = perpendicular to suspension
                    lat_nx = -sy; lat_ny = sx;
                }
            }
        }
        // Project current lateral velocity and cancel it
        float vx = finite_val(rb1.value("velocity_x", 0.f));
        float vy = finite_val(rb1.value("velocity_y", 0.f));
        float lat_vel = vx * lat_nx + vy * lat_ny;
        float j = -lat_vel / im1;
        j = std::clamp(j, -max_lateral * dt, max_lateral * dt);
        rb1["velocity_x"] = vx + j * im1 * lat_nx;
        rb1["velocity_y"] = vy + j * im1 * lat_ny;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  PLATFORM EFFECTOR sideArc — prevent wall-climbing on one-way platforms
//  Clamps friction on near-vertical contact normals so characters can't
//  climb the sides of one-way platforms. Matches Unity PlatformEffector2D's
//  sideArc property. Call from solve_velocities or apply_physics_ext.
// ════════════════════════════════════════════════════════════════════════════
// Applied at manifold creation time — marks manifolds with the side-friction
// scale so the solver uses it. Call after narrow_phase, before solve_velocities.
void apply_platform_side_friction(std::vector<Manifold>& manifolds, EntityList& entities) {
    for (auto& m : manifolds) {
        if (m.is_trigger) continue;
        // Check if either entity has a PlatformEffector2D with sideArc
        auto check = [&](Entity* ent) -> float {
            if (!ent) return 1.f;
            if (!has_component(*ent, "PlatformEffector2D")) return 1.f;
            auto& pe = (*ent)["components"]["PlatformEffector2D"];
            if (!pe.value("use_side_friction", false)) return 1.f;
            float side_arc = finite_val(pe.value("side_arc", 45.f)); // degrees from vertical
            float side_arc_rad = side_arc * (float)M_PI / 180.f;
            float cos_threshold = std::cos(side_arc_rad);
            // |nx| > cos_threshold means the normal is within side_arc of horizontal
            // i.e. this is a "side" contact → reduce friction to near zero
            if (std::abs(m.nx) > cos_threshold) return 0.05f; // tiny residual friction
            return 1.f;
        };
        float scale1 = check(m.e1);
        float scale2 = check(m.e2);
        float scale = std::min(scale1, scale2);
        if (scale < 1.f) {
            // Bake the scale into the manifold's friction for the solver to use
            m.friction_d *= scale;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  EFFECTOR LAYER MASK ENFORCEMENT for BuoyancyEffector2D
//  Wire _effector_layer_mask check into apply_buoyancy_effectors.
//  This is done by providing a patched re-entry point that checks the mask.
// ════════════════════════════════════════════════════════════════════════════
// Returns true if the dynamic body (with its layer) passes the effector's mask.
static bool effector_affects_layer(Entity& effector_entity,
                                    const std::string& effector_type, int body_layer)
{
    if (!has_component(effector_entity, effector_type)) return true;
    auto& ec = effector_entity["components"][effector_type];
    int mask = ec.value("_effector_layer_mask", 0xFFFF);
    return (mask & (1 << body_layer)) != 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  PER-ENTITY CONTACT CACHE INDEX
//  Provides O(1) lookup of manifolds per entity (instead of O(n) scan).
//  Rebuilt each frame inside apply_physics_ext after narrow phase.
//  get_contacts_fast(entity_id) returns all manifolds touching that body.
// ════════════════════════════════════════════════════════════════════════════
static std::unordered_map<int, std::vector<Manifold*>> s_contact_index;

static void rebuild_contact_index(std::vector<Manifold>& manifolds) {
    s_contact_index.clear();
    for (auto& m : manifolds) {
        if (m.is_trigger || m.contact_count == 0) continue;
        if (m.e1) s_contact_index[m.e1->value("id",0)].push_back(&m);
        if (m.e2) s_contact_index[m.e2->value("id",0)].push_back(&m);
    }
}

std::vector<Manifold*> get_contacts_fast(int entity_id) {
    auto it = s_contact_index.find(entity_id);
    if (it == s_contact_index.end()) return {};
    return it->second;
}

// O(1) contact count (uses the index)
int get_contact_count_fast(int entity_id) {
    auto it = s_contact_index.find(entity_id);
    if (it == s_contact_index.end()) return 0;
    int total = 0;
    for (auto* m : it->second) total += m->contact_count;
    return total;
}

// ════════════════════════════════════════════════════════════════════════════
//  DEBUG DRAW  (draw_debug_physics)
//  Implements the DebugDrawOptions stub. Calls user-supplied callbacks so
//  the engine/game layer can render using its own drawing API.
//  Register draw callbacks via set_debug_draw_callbacks() before calling.
// ════════════════════════════════════════════════════════════════════════════
static DebugDrawCallbacks s_debug_callbacks;

void set_debug_draw_callbacks(const DebugDrawCallbacks& cbs) {
    s_debug_callbacks = cbs;
}

void draw_debug_physics(EntityList& entities,
                        const std::vector<Manifold>& manifolds,
                        const DebugDrawOptions& opts)
{
    if (!s_debug_callbacks.draw_line && !s_debug_callbacks.draw_circle) return;

    auto line = [&](float x1,float y1,float x2,float y2,float r,float g,float b,float a=1.f){
        if (s_debug_callbacks.draw_line) s_debug_callbacks.draw_line(x1,y1,x2,y2,r,g,b,a);
    };
    auto circle = [&](float cx,float cy,float radius,float r,float g,float b,float a=1.f){
        if (s_debug_callbacks.draw_circle) s_debug_callbacks.draw_circle(cx,cy,radius,r,g,b,a);
    };
    auto text = [&](float x,float y,const std::string& t,float r,float g,float b,float a=1.f){
        if (s_debug_callbacks.draw_text) s_debug_callbacks.draw_text(x,y,t,r,g,b,a);
    };

    // Draw collider shapes
    for (auto& e : entities) {
        if (!entity_active(e)) continue;
        bool sleeping = has_component(e,"Rigidbody2D") &&
                        e["components"]["Rigidbody2D"].value("_sleeping",false);
        auto shapes = collect_shapes(e);
        for (auto& sh : shapes) {
            // Color: sleeping=grey, static=green, kinematic=blue, dynamic=white, trigger=cyan
            float cr=1,cg=1,cb=1,ca=0.8f;
            if (sh.col && sh.col->value("is_trigger",false)) { cr=0;cg=1;cb=1; }
            else if (sleeping) { cr=0.5f;cg=0.5f;cb=0.5f; }
            else if (has_component(e,"Rigidbody2D")) {
                std::string bt = body_type(e["components"]["Rigidbody2D"]);
                if (bt=="static")    { cr=0;cg=1;cb=0; }
                else if (bt=="kinematic") { cr=0;cg=0;cb=1; }
            }
            if (sh.kind == ShapeKind::Circle) {
                const int SEG = 16;
                for (int i=0;i<SEG;++i) {
                    float a0 = (float)i/(float)SEG*(2*(float)M_PI);
                    float a1 = (float)(i+1)/(float)SEG*(2*(float)M_PI);
                    line(sh.cx+std::cos(a0)*sh.radius, sh.cy+std::sin(a0)*sh.radius,
                         sh.cx+std::cos(a1)*sh.radius, sh.cy+std::sin(a1)*sh.radius,
                         cr,cg,cb,ca);
                }
                // Orientation indicator
                if (has_component(e,"Transform")) {
                    float rot = finite_val(get_float(e["components"]["Transform"],"rotation"))
                                *(float)M_PI/180.f;
                    line(sh.cx, sh.cy, sh.cx+std::cos(rot)*sh.radius*0.8f,
                         sh.cy+std::sin(rot)*sh.radius*0.8f, cr,cg,cb,ca);
                }
            } else if (sh.kind == ShapeKind::Polygon) {
                const auto& v = sh.verts;
                for (int i=0;i<(int)v.size();++i) {
                    auto [x1,y1] = v[i];
                    auto [x2,y2] = v[(i+1)%v.size()];
                    line(x1,y1,x2,y2,cr,cg,cb,ca);
                }
            } else if (sh.kind == ShapeKind::Capsule) {
                Vec2 A = sh.world_pts[0], B = sh.world_pts[1];
                float dx = B.first-A.first, dy = B.second-A.second;
                float len = std::hypot(dx,dy); if (len < 1e-6f) len = 1e-6f;
                float px = -dy/len * sh.radius, py = dx/len * sh.radius;
                line(A.first+px,A.second+py, B.first+px,B.second+py, cr,cg,cb,ca);
                line(A.first-px,A.second-py, B.first-px,B.second-py, cr,cg,cb,ca);
                const int HC = 8;
                for (int i=0;i<HC;++i) {
                    float a0 = (float)i/(float)HC*(float)M_PI;
                    float a1 = (float)(i+1)/(float)HC*(float)M_PI;
                    float ex = dx/len, ey = dy/len;
                    float nx2 = -ey, ny2 = ex;
                    // Cap at B
                    line(B.first + (std::cos(a0)*nx2 + std::sin(a0)*ex)*sh.radius,
                         B.second + (std::cos(a0)*ny2 + std::sin(a0)*ey)*sh.radius,
                         B.first + (std::cos(a1)*nx2 + std::sin(a1)*ex)*sh.radius,
                         B.second + (std::cos(a1)*ny2 + std::sin(a1)*ey)*sh.radius,
                         cr,cg,cb,ca);
                    // Cap at A
                    line(A.first + (std::cos(a0+M_PI)*nx2 + std::sin(a0+M_PI)*ex)*sh.radius,
                         A.second + (std::cos(a0+M_PI)*ny2 + std::sin(a0+M_PI)*ey)*sh.radius,
                         A.first + (std::cos(a1+M_PI)*nx2 + std::sin(a1+M_PI)*ex)*sh.radius,
                         A.second + (std::cos(a1+M_PI)*ny2 + std::sin(a1+M_PI)*ey)*sh.radius,
                         cr,cg,cb,ca);
                }
            }
            // AABB overlay
            if (opts.draw_aabbs) {
                auto [ax1,ay1,ax2,ay2] = shape_aabb(sh);
                line(ax1,ay1,ax2,ay1,0.5f,0.5f,0.5f,0.4f);
                line(ax2,ay1,ax2,ay2,0.5f,0.5f,0.5f,0.4f);
                line(ax2,ay2,ax1,ay2,0.5f,0.5f,0.5f,0.4f);
                line(ax1,ay2,ax1,ay1,0.5f,0.5f,0.5f,0.4f);
            }
        }
        // Sleep state label
        if (opts.draw_ground_info && sleeping && has_component(e,"Transform")) {
            auto& tr = e["components"]["Transform"];
            text(finite_val(get_float(tr,"x")), finite_val(get_float(tr,"y")),
                 "ZZZ", 0.5f,0.5f,0.5f,0.6f);
        }
    }

    // Draw contact points and normals
    if (opts.draw_contacts) {
        for (auto& m : manifolds) {
            if (m.is_trigger) continue;
            for (int ci = 0; ci < m.contact_count; ++ci) {
                const auto& cp = m.contacts[ci];
                circle(cp.x, cp.y, 2.f, 1.f, 0.f, 0.f, 1.f);  // red dot
                // Normal arrow
                float scale = std::min(cp.depth * 4.f + 8.f, 24.f);
                line(cp.x, cp.y, cp.x + m.nx*scale, cp.y + m.ny*scale,
                     1.f, 0.5f, 0.f, 0.9f);  // orange
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  queriesStartInColliders enforcement for raycast / overlap queries
//  Call check_queries_start(ox,oy,entity,shape) inside raycasts to decide
//  whether to skip a hit that starts inside the collider.
// ════════════════════════════════════════════════════════════════════════════
// Returns true if the query origin is inside the shape AND queriesStartInColliders is false
// (meaning we should SKIP this hit).
bool should_skip_start_in_collider(float ox, float oy, const Shape& sh) {
    if (s_queries_start_in_colliders) return false; // flag is on: allow start-in hits
    // Test if origin is inside the shape
    switch (sh.kind) {
    case ShapeKind::Circle: {
        float dx = ox - sh.cx, dy = oy - sh.cy;
        return (dx*dx + dy*dy) <= sh.radius * sh.radius;
    }
    case ShapeKind::Polygon:
        return point_in_poly(ox, oy, sh.verts);
    case ShapeKind::Capsule: {
        auto [closest, d2] = closest_on_seg(
            sh.world_pts[0].first, sh.world_pts[0].second,
            sh.world_pts[1].first, sh.world_pts[1].second, ox, oy);
        return d2 <= sh.radius * sh.radius;
    }
    default: return false;
    }
}

} // namespace phys