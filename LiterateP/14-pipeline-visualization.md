# Chapter 14: Visualizing the Pipeline — PipelineStageRenderer and the Pipeline Steps Window

**Abstract.** Chapter 13 gave the app a rasterizer; this chapter gives it a way to *watch* the
rasterizer's own camera math happen, one stage at a time. `PipelineStageRenderer` renders three
wireframe diagrams — "Projective Matrix," "Camera Matrix," "Orthographic Matrix" — showing the
scene at each intermediate coordinate space a traditional graphics pipeline passes through before
the final "Viewport Matrix" image Chapter 13 already produces; `PipelineVisualizationWindow` is the
optional secondary AppKit window that displays all four side by side. This chapter is unusual among
this document's chapters in that its final form was shaped by two rounds of real user feedback
after the first version shipped, and both rounds are worth reading for the same reason Chapter 7's
export-corruption story is: the bug (or in this case, the wrong picture) is more instructive than
the fix that replaced it.

Files covered: `GPU/PipelineStageRenderer.hpp`, `GPU/PipelineStageRenderer.cpp`,
`Shaders/Wireframe.metal`, `App/PipelineVisualizationWindow.hpp`, `App/PipelineVisualizationWindow.cpp`,
plus the `NS::Window::setReleasedWhenClosed()` addition to
`ThirdParty/metal-cpp-extensions/AppKit/NSWindow.hpp`.

---

## §1. Four stages, three of them new

The user's original request named the four stages in this exact order, with a description of what
each one should show:

> The steps are: Projective Matrix, Camera Matrix, Orthographic Matrix, Viewport Matrix. The
> Projective Matrix step should show the position of the camera, the position and orientation of
> the image plane, and the objects in 3D, deformed so they can be painted into the image plane. The
> Camera matrix step should show the camera in (0,0,0), looking at -Z and with Y up, and the rest of
> the scene transformed accordingly. The Orthographic matrix step should show the resulting image
> after applying the orthographic transformation to the previous scene, in an image with coordinates
> between -1 and 1 in X and Y. The Viewport Matrix scene should show the final image rendered by
> this method, with the appropriate dimensions.

The fourth stage needed no new rendering code at all — it is exactly `RasterRenderer::render()`
(Chapter 13), called through a second, independent `RasterRenderer` instance so a "Refresh" click in
this window can never race a `Render Raster`/`Compare` click on the main window over shared GPU
state (§6). The other three needed a class of their own, since none of them are "run the real
render at a different setting" — each is a different camera looking at a different transform of the
same underlying geometry.

## §2. A legible wireframe, not the real tessellated mesh

All three new stages need *some* representation of "the objects in 3D" to transform and draw. Using
`buildEntityMesh()`'s full tessellated output (Chapter 6) directly — as Chapter 13's rasterizer
does — would mean drawing hundreds of thousands of line segments for the default scene's ~490
entities, an unreadable tangle rather than a diagram. `buildSceneWireframe()` instead builds a
deliberately reduced proxy:

```cpp
// GPU/PipelineStageRenderer.cpp:260-303
std::vector<LineVertex> buildSceneWireframe( const SceneDescription& scene, bool excludeOversizedEntities )
{
	std::vector<LineVertex> lines;

	for ( size_t e = 0; e < scene.transforms.size(); ++e )
	{
		const TransformGPU& transform = scene.transforms[ e ];
		const ShapeGPU&      shape = scene.shapes[ e ];

		if ( excludeOversizedEntities && entityBoundingRadius( shape ) > kFramingSizeExcludeThreshold )
			continue;

		simd_float3 color = colorForMaterial( scene.materials[ shape.materialIndex ] );

		if ( shape.type == SHAPE_SPHERE )
		{
			for ( int i = 0; i < kSphereRingSegments; ++i )
			{
				float angleA = 2.0f * kPi * (float)i / (float)kSphereRingSegments;
				float angleB = 2.0f * kPi * (float)( i + 1 ) / (float)kSphereRingSegments;

				simd_float3 a = transform.position + shape.radius * ( std::cos( angleA ) * transform.right + std::sin( angleA ) * transform.forward );
				simd_float3 b = transform.position + shape.radius * ( std::cos( angleB ) * transform.right + std::sin( angleB ) * transform.forward );
				appendLine( lines, a, b, color );
			}
		}
		else
		{
			MeshData mesh = buildEntityMesh( transform, shape );
			for ( size_t i = 0; i + 2 < mesh.indices.size(); i += 3 )
			{
				simd_float3 a = mesh.positions[ mesh.indices[ i ] ];
				simd_float3 b = mesh.positions[ mesh.indices[ i + 1 ] ];
				simd_float3 c = mesh.positions[ mesh.indices[ i + 2 ] ];
				appendLine( lines, a, b, color );
				appendLine( lines, b, c, color );
				appendLine( lines, c, a, color );
			}
		}
	}

	return lines;
}
```

Pyramids — only 5 entities, already low-poly (18 verts/6 tris, Chapter 6) — get their exact
triangle-edge wireframe straight from `buildEntityMesh()`. Spheres do not: instead of their full
tessellated mesh, each gets a single `kSphereRingSegments`-segment equatorial ring at its actual
center/radius. This is only exact because spheres always use the identity orientation basis
(`Scene.cpp`'s `identityTransformAt()`, Chapter 1) — "equatorial" is unambiguous only because
`transform.right`/`transform.forward` are always world X/Z for a sphere, never an arbitrary
rotation.

The `excludeOversizedEntities` parameter, and `kFramingSizeExcludeThreshold` (50 world units) it
checks against, exist for a reason this chapter's second round of feedback (§4) actually surfaced —
they are described together with that story rather than here, since the "why" only makes sense once
the export/framing problem it solves has been shown.

## §3. Framing an external observer camera at a scene it isn't the scene's own camera

The Projective- and Camera-Matrix stages both need to look *at* the scene's own camera from
outside, not through it — an "external observer" viewpoint with no natural default position. Both
stages auto-frame this observer to the same simple recipe: collect a set of points worth keeping in
view, fit the smallest reasonable sphere around them, and place a camera far enough back (in a
fixed, arbitrary elevated direction) that the whole sphere fits inside a chosen field of view.

```cpp
// GPU/PipelineStageRenderer.cpp:305-320
FramingBounds fitFramingSphere( const std::vector<simd_float3>& points )
{
	if ( points.empty() )
		return FramingBounds{ simd_make_float3( 0.0f, 0.0f, 0.0f ), 1.0f };

	simd_float3 sum = simd_make_float3( 0.0f, 0.0f, 0.0f );
	for ( simd_float3 p : points )
		sum += p;
	simd_float3 center = sum / (float)points.size();

	float radius = 0.0f;
	for ( simd_float3 p : points )
		radius = std::max( radius, simd_length( p - center ) );

	return FramingBounds{ center, radius };
}
```

This is not a minimal enclosing sphere (centroid-and-farthest-point can be beaten by a smarter
algorithm for pathological point sets) — it is simple, deterministic, and good enough for camera
framing, which is the only thing it's used for. `buildObserverViewProjection()` turns one
`FramingBounds` into an actual view-projection matrix using the standard fit-a-sphere-in-a-cone
formula (distance = radius / sin(halfFOV)) from a fixed 3/4-elevated direction, reusing
`CameraMath.hpp`'s `buildViewMatrix()`/`buildPerspectiveProjection()` primitives (Chapter 13, §5) —
the reason those were factored out of `RasterRenderer.cpp` in the first place:

```cpp
// GPU/PipelineStageRenderer.cpp:120-143
simd_float4x4 buildObserverViewProjection( const FramingBounds& bounds, float aspect )
{
	constexpr float kVFovDegrees = 40.0f;
	constexpr float kMarginFactor = 1.25f;

	float radius = std::max( bounds.radius, 0.01f ) * kMarginFactor;
	float halfVFov = ( kVFovDegrees * kPi / 180.0f ) * 0.5f;
	float distance = radius / std::sin( halfVFov );

	simd_float3 dir = simd_normalize( simd_make_float3( 0.6f, 0.5f, 1.0f ) );
	simd_float3 lookFrom = bounds.center + dir * distance;

	simd_float3 back = dir; // lookFrom - lookAt, normalized — already `dir` by construction
	simd_float3 worldUp = simd_make_float3( 0.0f, 1.0f, 0.0f );
	simd_float3 right = simd_normalize( simd_cross( worldUp, back ) );
	simd_float3 up = simd_cross( back, right );

	float halfVFovTan = std::tan( halfVFov );
	float halfHFovTan = halfVFovTan * aspect;

	simd_float4x4 view = buildViewMatrix( lookFrom, right, up, back );
	simd_float4x4 proj = buildPerspectiveProjection( halfHFovTan, halfVFovTan, 0.05f, distance * 4.0f + radius * 4.0f );
	return simd_mul( proj, view );
}
```

## §4. First round of feedback: the Unity-import lesson, encoded before it could repeat

`collectFramingPoints()` decides which points feed `fitFramingSphere()`:

```cpp
// GPU/PipelineStageRenderer.cpp:322-332
std::vector<simd_float3> collectFramingPoints( const SceneDescription& scene, const std::vector<simd_float3>& extraPoints )
{
	std::vector<simd_float3> points;
	for ( size_t e = 0; e < scene.transforms.size(); ++e )
	{
		if ( entityBoundingRadius( scene.shapes[ e ] ) <= kFramingSizeExcludeThreshold )
			points.push_back( scene.transforms[ e ].position );
	}
	points.insert( points.end(), extraPoints.begin(), extraPoints.end() );
	return points;
}
```

The exclusion this function applies — skip any entity whose own bounding radius tops
`kFramingSizeExcludeThreshold` (50 units) — is not a guess; it is this session's *second* piece of
work applied to a *first* piece of work's lesson, before the mistake had a chance to recur. Earlier
in the same session, the user asked why an exported `.gltf`/`.obj` looked like every object had
collapsed onto a single point once opened in Unity. The actual cause turned out to be a scale
illusion, not a bug in the export: the default scene's ground sphere has radius 1000 against a field
of radius-0.2 spheres spanning roughly 22 units, and any camera that tries to frame *both* zooms out
so far that the small, interesting objects shrink to a few pixels while the enormous ground sphere
dominates the shot. `entityBoundingRadius()` is the reusable per-entity size check this diagnosis
produced:

```cpp
// GPU/PipelineStageRenderer.cpp:249-258
float entityBoundingRadius( const ShapeGPU& shape )
{
	if ( shape.type == SHAPE_SPHERE )
		return shape.radius;
	// Pyramid: farthest of the 5 local vertices (apex + 4 base corners) from the local origin.
	float apexDist = shape.height;
	float cornerDist = shape.baseHalfWidth * 1.41421356f; // sqrt(2)
	return std::max( apexDist, cornerDist );
}
```

When this window's own external-observer cameras were written, the same illusion was one line of
code away from happening again — an unfiltered `collectFramingPoints()` would let the ground
sphere's radius-1000 extent dictate how far back the observer camera sits, and every small object
would collapse to a speck exactly the way the Unity import once did. `kFramingSizeExcludeThreshold`
exists specifically so that lesson gets applied instead of relearned:

```cpp
// GPU/PipelineStageRenderer.hpp:22-28
// Entities whose own local bounding radius exceeds this are excluded from framing-bounds fitting
// (see fitFramingSphere()) but still drawn in the wireframe itself. Without this, an "external
// observer" camera trying to fit the whole scene — including the ground sphere's radius-1000 extent
// against a ~22-unit field of radius-0.2 spheres — would zoom out so far that every "interesting"
// object collapses to a few pixels: the exact illusion this session's Unity-import investigation
// diagnosed (see CLAUDE.md), now deliberately avoided here instead of reproduced.
constexpr float kFramingSizeExcludeThreshold = 50.0f;
```

Note the two functions' different relationships to this threshold: `collectFramingPoints()` (used
by both the Camera-Matrix stage and, transformed, the framing math below) excludes the oversized
entity from *deciding how the camera is framed* but says nothing about drawing it, while
`buildSceneWireframe()`'s own `excludeOversizedEntities` flag (§2) is a separate decision about
whether to *draw* it at all — the Camera-Matrix stage passes `true` for both (§7), while the
Orthographic-Matrix stage passes neither (§8), because after a full perspective divide the ground
sphere's projection no longer dominates the frame the way its world-space wireframe would.

## §5. Ray/plane projection: the "deformed" image

`projectOntoPlane()` is one ray/plane intersection, used to show what a 3D point looks like once
cast through the picture plane onto it:

```cpp
// GPU/PipelineStageRenderer.cpp:334-340
simd_float3 projectOntoPlane( simd_float3 origin, simd_float3 point, simd_float3 planePoint, simd_float3 planeNormal )
{
	simd_float3 direction = point - origin;
	float       denom = simd_dot( direction, planeNormal );
	float       t = simd_dot( planePoint - origin, planeNormal ) / denom;
	return origin + t * direction;
}
```

Points behind `origin` aren't clipped — an explicit, documented scope limit, since this app's own
camera setups never put scene geometry behind themselves, so it never arises in practice for the
default scene. What *does* arise in practice, and very nearly derailed this stage's second design
pass, is covered in §6.

## §6. Second round of feedback: a reference image, a redesign, and a numerically-diagnosed bug

The first version of the Projective-Matrix stage drew fixed frustum edges from the camera to the
image plane's own four corners — the plane rectangle `CameraGPU` already defines, unrelated to
where the scene's actual objects sit — plus a dense per-vertex projection of the whole wireframe
onto that plane. The user compared the result against a real perspective-drawing textbook
illustration (an eye, straight sight lines to an object, a picture plane the lines pass through with
the flattened result traced on it, and a ground/reference plane) and said plainly that the
projective view was wrong. The two diagrams *were* different in a way that mattered: the reference's
sight lines connect the eye to actual points on the object being drawn; the first version's frustum
lines connected the eye to the plane's own corners, which have nothing to do with the object at all,
and there was no line anywhere showing an object point being cast onto its projected counterpart.

The redesign replaced "draw the scene's dense wireframe, plus a separate fixed frustum, plus a
dense projected copy" with a curated object stand-in and real sight lines to it — the same
one-level-up curation §2 already applies to individual spheres, generalized to the whole visible
scene:

```cpp
// GPU/PipelineStageRenderer.cpp:145-151
// An axis-aligned box, used as the "object" the classic eye/sight-line/picture-plane diagram
// (see the reference image the user linked) draws sight lines to — the same curated-proxy idea
// buildSceneWireframe() already uses for spheres (a ring instead of a full tessellated mesh),
// applied one level up: with ~490 entities, drawing sight lines to *every* vertex would be a
// solid fan of thousands of rays, nothing like the reference's handful of clean lines. The box
// covers every non-excluded entity's actual extent (center ± its own bounding radius, not just
// its center point), so it's a true bound on "the object" rather than a looser center-only box.
```

The very first version of this box, though, produced a *new* problem: several long, near-parallel
lines shooting past the diagram's frame edge, which the user found by literally circling them in a
screenshot and asking what they were. This was root-caused numerically, not guessed at — a
standalone harness printed each of the object box's 8 corners' ray/plane intersection parameter
against the real default scene, and found two corners whose denominator was nearly zero (`-0.64`
and `-0.16`, against typical values around `-25`), producing projected points hundreds of units from
a scene whose real extent is roughly 20 units — one landed at `(-111, 2, 495)`. The cause was that
an axis-aligned box built from *every* non-excluded entity spans a much wider area (~22 units) than
the camera's ~20°-vertical-FOV actually captures at this distance, so the box's own corners are
*combinatorial*: one sphere's extreme +X value mixed with a completely different, distant sphere's
extreme +Z value, a synthetic point no real entity occupies, which can land almost exactly edge-on
to the camera (the sight-line ray nearly parallel to the picture plane itself).

The fix operates on two levels — a principled one and a defensive backstop, since the
combinatorial-corner problem can't be fully eliminated by entity-level filtering alone. First,
`computeObjectBox()` restricts which entities can contribute a corner to only those actually within
the camera's real field of view:

```cpp
// GPU/PipelineStageRenderer.cpp:159-221
bool isRoughlyInCameraView( const CameraGPU& cam, simd_float3 point, float halfHFovTan, float halfVFovTan )
{
	constexpr float kFovMargin = 1.0f; // exactly the camera's own FOV, no extra slack — see the comment above

	simd_float3 direction = point - cam.origin;
	float       depthAlongView = -simd_dot( direction, cam.w ); // cam.w is "backward", so forward depth is -dot
	if ( depthAlongView <= 0.01f )
		return false; // behind the camera

	float rightFrac = simd_dot( direction, cam.u ) / ( depthAlongView * halfHFovTan );
	float upFrac = simd_dot( direction, cam.v ) / ( depthAlongView * halfVFovTan );
	return std::fabs( rightFrac ) <= kFovMargin && std::fabs( upFrac ) <= kFovMargin;
}

AxisAlignedBox computeObjectBox( const SceneDescription& scene )
{
	const CameraGPU& cam = scene.camera;
	simd_float3       viewportCenter = cam.lowerLeftCorner + cam.horizontal * 0.5f + cam.vertical * 0.5f;
	float             focusDist = simd_dot( cam.origin - viewportCenter, cam.w );
	float             halfVFovTan = ( simd_length( cam.vertical ) * 0.5f ) / focusDist;
	float             halfHFovTan = ( simd_length( cam.horizontal ) * 0.5f ) / focusDist;

	AxisAlignedBox box{ simd_make_float3( 0.0f, 0.0f, 0.0f ), simd_make_float3( 0.0f, 0.0f, 0.0f ), false };

	for ( size_t e = 0; e < scene.transforms.size(); ++e )
	{
		float radius = entityBoundingRadius( scene.shapes[ e ] );
		if ( radius > kFramingSizeExcludeThreshold )
			continue;
		if ( !isRoughlyInCameraView( cam, scene.transforms[ e ].position, halfHFovTan, halfVFovTan ) )
			continue;

		simd_float3 rvec = simd_make_float3( radius, radius, radius );
		simd_float3 lo = scene.transforms[ e ].position - rvec;
		simd_float3 hi = scene.transforms[ e ].position + rvec;

		if ( !box.valid )
		{
			box.minCorner = lo;
			box.maxCorner = hi;
			box.valid = true;
		}
		else
		{
			box.minCorner = simd_min( box.minCorner, lo );
			box.maxCorner = simd_max( box.maxCorner, hi );
		}
	}

	return box;
}
```

The margin was tuned empirically, not assumed: a first attempt at 1.5× the camera's actual half-FOV
tangent still let one corner project 20+ units off; tightening it to exactly 1.0× (no slack at all)
brought the worst case down to roughly 2 units of overshoot — much better, but the numerical
investigation that found this also showed that box-corner combinatorics can't be bounded away
entirely by entity-level filtering alone, since the box's corners remain synthetic no matter how
tightly the *contributing entities* are scoped. The second layer of the fix is therefore a defensive
check on the corners themselves, applied only where a corner is actually about to be projected and
drawn:

```cpp
// GPU/PipelineStageRenderer.cpp:532-552
// The "deformed" shape: the box's own corners, ray-cast through the picture plane, redrawn with
// the box's own edge connectivity — the flattened outline traced on the plane, directly
// analogous to the reference image's projected horse silhouette. Even with the FOV-scoped box
// above, an axis-aligned box's corners are still *combinatorial* (e.g. one entity's extreme +X
// mixed with a different entity's extreme +Z), so a corner can still occasionally sit at a
// shallow, near-edge-on angle to the camera — this is a defensive backstop, not the primary
// fix: skip (rather than draw) any edge whose ray/plane intersection parameter is out of a sane
// range, so an unlucky corner can't paint a line shooting off to an absurd distance.
simd_float3 projectedCorners[ 8 ];
bool        cornerProjectionValid[ 8 ];
for ( int i = 0; i < 8; ++i )
{
	simd_float3 direction = corners[ i ] - cam.origin;
	float       denom = simd_dot( direction, cam.w );
	float       t = simd_dot( c00 - cam.origin, cam.w ) / denom;
	cornerProjectionValid[ i ] = ( t > 0.0f && t < 3.0f );
	projectedCorners[ i ] = cam.origin + t * direction;
}
for ( const auto& edge : kBoxEdges )
	if ( cornerProjectionValid[ edge[ 0 ] ] && cornerProjectionValid[ edge[ 1 ] ] )
		appendLine( lines, projectedCorners[ edge[ 0 ] ], projectedCorners[ edge[ 1 ] ], kDeformedColor );
```

Note what this guard does *not* touch: the sight lines themselves (§7 below), drawn straight from
the eye to the real 3D corner, are simple finite line segments with no division involved and can
never blow up regardless of viewing angle — only the plane *projection* of a corner is at risk, and
only that projection is guarded.

## §7. `renderProjectiveStage()`: the finished composition

Putting §2–§6 together, the Projective-Matrix stage draws, in this order: the dimmed context
wireframe (§2, oversized entities excluded), a schematic ground/reference plane sized to the object
box's footprint (replacing the actual ground sphere, which — undimmed and undiminished by a diagram
this small — reads as noise rather than "ground," since its top touches world Y=0 right where the
rest of the scene sits), the object box itself, the image-plane rectangle, a camera icon, the sight
lines, and finally the guarded deformed outline:

```cpp
// GPU/PipelineStageRenderer.cpp:465-558 (elided: box/plane/edge construction shown in §4-§6 above)
PipelineStageResult PipelineStageRenderer::renderProjectiveStage( const SceneDescription& scene, uint32_t width, uint32_t height )
{
	rebuildTargetIfNeeded( &_pProjectiveTexture, width, height );

	const CameraGPU& cam = scene.camera;
	simd_float3       c00 = cam.lowerLeftCorner;
	simd_float3       c10 = cam.lowerLeftCorner + cam.horizontal;
	simd_float3       c01 = cam.lowerLeftCorner + cam.vertical;
	simd_float3       c11 = cam.lowerLeftCorner + cam.horizontal + cam.vertical;

	std::vector<LineVertex> lines = buildSceneWireframe( scene, /*excludeOversizedEntities=*/true );
	for ( LineVertex& v : lines )
		v.color = v.color * kContextWireframeDimFactor;

	std::vector<simd_float3> framingPoints = collectFramingPoints( scene, { cam.origin, c00, c10, c01, c11 } );
	FramingBounds            bounds = fitFramingSphere( framingPoints );

	AxisAlignedBox objectBox = computeObjectBox( scene );
	/* ... ground plane, object box edges, image-plane rectangle, camera icon, sight lines,
	       and the guarded deformed outline — each shown in full in §4-§6 above ... */

	simd_float4x4 mvp = buildObserverViewProjection( bounds, (float)width / (float)height );
	drawLines( _pProjectiveTexture, lines, mvp, width, height );

	return PipelineStageResult{ _pProjectiveTexture };
}
```

The camera icon itself is a small axis triad — right/up/forward drawn as short red/green/blue
segments from `cam.origin`, sized relative to the framing radius so it stays visible regardless of
scene scale:

```cpp
// GPU/PipelineStageRenderer.cpp:65-72
void appendAxisTriad( std::vector<LineVertex>& lines, simd_float3 origin, simd_float3 right, simd_float3 up, simd_float3 forward, float scale )
{
	appendLine( lines, origin, origin + right * scale, kAxisRightColor );
	appendLine( lines, origin, origin + up * scale, kAxisUpColor );
	appendLine( lines, origin, origin + forward * scale, kAxisForwardColor );
}
```

## §8. The Camera- and Orthographic-Matrix stages: transform first, then reuse the same pieces

Once the wireframe/framing/drawing machinery above exists, the remaining two stages are short.
`renderCameraStage()` transforms the same world-space wireframe by the scene camera's own view
matrix — placing the camera at the origin looking down `-Z` with `+Y` up *by construction of the
matrix itself*, not by any special-casing — and draws both a coordinate-axis triad and a small
camera-body "gizmo" there, a second piece of user-requested follow-up polish (a plain axis triad
alone reads as "three colored lines," not obviously "a camera"):

```cpp
// GPU/PipelineStageRenderer.cpp:560-596
PipelineStageResult PipelineStageRenderer::renderCameraStage( const SceneDescription& scene, uint32_t width, uint32_t height )
{
	rebuildTargetIfNeeded( &_pCameraTexture, width, height );

	const CameraGPU& cam = scene.camera;
	simd_float4x4    view = buildViewMatrix( cam.origin, cam.u, cam.v, cam.w );

	// Same exclusion as the Projective-Matrix stage — see buildSceneWireframe()'s header comment.
	std::vector<LineVertex> worldLines = buildSceneWireframe( scene, /*excludeOversizedEntities=*/true );
	std::vector<LineVertex> lines;
	lines.reserve( worldLines.size() );
	for ( const LineVertex& v : worldLines )
		lines.push_back( LineVertex{ transformPointRigid( view, v.position ), v.color } );

	std::vector<simd_float3> framingPoints = collectFramingPoints( scene, { cam.origin } );
	std::vector<simd_float3> transformedFramingPoints;
	transformedFramingPoints.reserve( framingPoints.size() );
	for ( simd_float3 p : framingPoints )
		transformedFramingPoints.push_back( transformPointRigid( view, p ) );
	FramingBounds bounds = fitFramingSphere( transformedFramingPoints );

	simd_float3 originPoint = simd_make_float3( 0.0f, 0.0f, 0.0f );
	simd_float3 unitRight = simd_make_float3( 1.0f, 0.0f, 0.0f );
	simd_float3 unitUp = simd_make_float3( 0.0f, 1.0f, 0.0f );
	simd_float3 unitForward = simd_make_float3( 0.0f, 0.0f, -1.0f );
	appendAxisTriad( lines, originPoint, unitRight, unitUp, unitForward, bounds.radius * 0.08f );
	appendCameraGizmo( lines, originPoint, unitRight, unitUp, unitForward, bounds.radius * 0.15f, kCameraGizmoColor );

	simd_float4x4 mvp = buildObserverViewProjection( bounds, (float)width / (float)height );
	drawLines( _pCameraTexture, lines, mvp, width, height );

	return PipelineStageResult{ _pCameraTexture };
}
```

`transformPointRigid()` applies the view matrix and takes only the `.xyz` of the result — a
rotation+translation never introduces a perspective `w`, so no divide is needed, unlike §9's
`transformPointPerspective()`. The framing points are collected in world space and then transformed
by the *same* view matrix, so the observer camera for this stage frames the post-transform scene
correctly rather than reusing stage 1's (now-irrelevant) world-space framing. `appendCameraGizmo()`
draws a small wireframe pyramid — apex at the origin, a small rectangle out along the forward
axis — the same convention many 3D tools use for a camera icon:

```cpp
// GPU/PipelineStageRenderer.cpp:74-98
void appendCameraGizmo( std::vector<LineVertex>& lines, simd_float3 origin, simd_float3 right, simd_float3 up, simd_float3 forward, float size, simd_float3 color )
{
	simd_float3 baseCenter = origin + forward * size;
	float       halfWidth = size * 0.6f;
	float       halfHeight = size * 0.4f;

	simd_float3 c00 = baseCenter - right * halfWidth - up * halfHeight;
	simd_float3 c10 = baseCenter + right * halfWidth - up * halfHeight;
	simd_float3 c11 = baseCenter + right * halfWidth + up * halfHeight;
	simd_float3 c01 = baseCenter - right * halfWidth + up * halfHeight;

	appendLine( lines, origin, c00, color );
	appendLine( lines, origin, c10, color );
	appendLine( lines, origin, c11, color );
	appendLine( lines, origin, c01, color );

	appendLine( lines, c00, c10, color );
	appendLine( lines, c10, c11, color );
	appendLine( lines, c11, c01, color );
	appendLine( lines, c01, c00, color );
}
```

`renderOrthographicStage()` is the simplest of the three: reuse `CameraMath.hpp`'s
`buildSceneViewProjectionMatrix()` unchanged (Chapter 13, §5 — the same matrix `RasterRenderer`
itself uses, so this stage is a faithful intermediate step of the exact same pipeline, not a
lookalike approximation), transform every wireframe vertex through it *with* a perspective divide
this time, and feed the result to `drawLines()` with an identity MVP, since the data is already
normalized:

```cpp
// GPU/PipelineStageRenderer.cpp:598-625
PipelineStageResult PipelineStageRenderer::renderOrthographicStage( const SceneDescription& scene, uint32_t width, uint32_t height )
{
	rebuildTargetIfNeeded( &_pOrthographicTexture, width, height );

	simd_float4x4 vp = buildSceneViewProjectionMatrix( scene.camera );

	std::vector<LineVertex> worldLines = buildSceneWireframe( scene );
	std::vector<LineVertex> lines;
	lines.reserve( worldLines.size() + 8 );
	for ( const LineVertex& v : worldLines )
		lines.push_back( LineVertex{ transformPointPerspective( vp, v.position ), v.color } );

	// Reference square at exactly the [-1,1] bounds the user asked to see explicitly.
	simd_float3 bl = simd_make_float3( -1.0f, -1.0f, 0.0f );
	simd_float3 br = simd_make_float3( 1.0f, -1.0f, 0.0f );
	simd_float3 tr = simd_make_float3( 1.0f, 1.0f, 0.0f );
	simd_float3 tl = simd_make_float3( -1.0f, 1.0f, 0.0f );
	appendLine( lines, bl, br, kReferenceSquareColor );
	appendLine( lines, br, tr, kReferenceSquareColor );
	appendLine( lines, tr, tl, kReferenceSquareColor );
	appendLine( lines, tl, bl, kReferenceSquareColor );

	simd_float4x4 identity = simd_matrix( simd_make_float4( 1, 0, 0, 0 ), simd_make_float4( 0, 1, 0, 0 ),
		simd_make_float4( 0, 0, 1, 0 ), simd_make_float4( 0, 0, 0, 1 ) );
	drawLines( _pOrthographicTexture, lines, identity, width, height );

	return PipelineStageResult{ _pOrthographicTexture };
}
```

This stage calls `buildSceneWireframe( scene )` with no `excludeOversizedEntities` argument (§2's
default of `false`) — after a full perspective divide, the ground sphere's own NDC-space footprint
no longer dominates the frame the way its raw world-space wireframe would in the other two stages,
so there was no visual reason to exclude it here. The reference square drawn at exactly
`(-1,-1)`–`(1,1)` makes the `[-1,1]` bounds the user asked to see literal rather than merely
implied by the data's own range.

## §9. Wireframe.metal: the smallest possible shader for colored lines

`drawLines()` (used by all three stages above) drives one shared render pipeline over
`Shaders/Wireframe.metal`, quoted here in full — it is, deliberately, the simplest shader in the
whole project:

```metal
// Shaders/Wireframe.metal:14-48
struct LineVertex
{
	float3 position;
	float3 color;
};

struct Uniforms
{
	float4x4 mvp;
};

struct RasterizerData
{
	float4 position [[position]];
	float3 color;
};

vertex RasterizerData wireframeVertex( uint vertexID [[vertex_id]],
	constant LineVertex* vertices [[buffer( 0 )]],
	constant Uniforms& uniforms [[buffer( 1 )]] )
{
	LineVertex in = vertices[ vertexID ];

	RasterizerData out;
	out.position = uniforms.mvp * float4( in.position, 1.0 );
	out.color = in.color;
	return out;
}

fragment float4 wireframeFragment( RasterizerData in [[stage_in]] )
{
	return float4( in.color, 1.0 );
}
```

No normals, no lighting, no depth test in the pipeline that draws this (§10) — this is a schematic
diagram, not a solid-occlusion render, so overlapping lines are expected and meant to show through
each other rather than being hidden. `LineVertex`/`Uniforms` are hand-duplicated between this file
and `PipelineStageRenderer.cpp` rather than shared through a header, marked `KEEP IN SYNC` on both
sides — the same precedent `Blit.metal`'s `MagnifierUniforms` set (Chapter 10) and `Raster.metal`'s
own `RasterVertex`/`Uniforms` repeat (Chapter 13, §6).

## §10. `drawLines()`: a non-indexed line list, no depth test

The pipeline state built in the constructor omits a depth-stencil state entirely (unlike Chapter
13's rasterizer), and each draw is one non-indexed `MTL::PrimitiveTypeLine` call over whatever
`std::vector<LineVertex>` the calling stage assembled:

```cpp
// GPU/PipelineStageRenderer.cpp:415-455
double PipelineStageRenderer::drawLines( MTL::Texture* pTexture, const std::vector<LineVertex>& lines, simd_float4x4 mvp, uint32_t width, uint32_t height )
{
	Uniforms uniforms{ mvp };

	MTL::Buffer* pVertexBuffer = nullptr;
	if ( !lines.empty() )
	{
		const size_t bytes = lines.size() * sizeof( LineVertex );
		pVertexBuffer = _pDevice->newBuffer( bytes, MTL::ResourceStorageModeShared );
		std::memcpy( pVertexBuffer->contents(), lines.data(), bytes );
	}

	MTL::RenderPassDescriptor*                pPassDesc = MTL::RenderPassDescriptor::renderPassDescriptor();
	MTL::RenderPassColorAttachmentDescriptor* pColorAttachment = pPassDesc->colorAttachments()->object( 0 );
	pColorAttachment->setTexture( pTexture );
	pColorAttachment->setLoadAction( MTL::LoadActionClear );
	pColorAttachment->setStoreAction( MTL::StoreActionStore );
	pColorAttachment->setClearColor( MTL::ClearColor::Make( 0.05, 0.05, 0.08, 1.0 ) );

	MTL::CommandBuffer*        pCommandBuffer = _pCommandQueue->commandBuffer();
	MTL::RenderCommandEncoder* pEncoder = pCommandBuffer->renderCommandEncoder( pPassDesc );

	pEncoder->setRenderPipelineState( _pPipelineState );
	pEncoder->setViewport( MTL::Viewport{ 0.0, 0.0, (double)width, (double)height, 0.0, 1.0 } );
	if ( pVertexBuffer )
	{
		pEncoder->setVertexBuffer( pVertexBuffer, 0, 0 );
		pEncoder->setVertexBytes( &uniforms, sizeof( Uniforms ), 1 );
		pEncoder->drawPrimitives( MTL::PrimitiveTypeLine, (NS::UInteger)0, (NS::UInteger)lines.size() );
	}
	pEncoder->endEncoding();

	pCommandBuffer->commit();
	pCommandBuffer->waitUntilCompleted();

	if ( pVertexBuffer )
		pVertexBuffer->release();

	return ( pCommandBuffer->GPUEndTime() - pCommandBuffer->GPUStartTime() ) * 1000.0;
}
```

Each of the class's three public `render*Stage()` methods owns its own output texture
(`_pProjectiveTexture`/`_pCameraTexture`/`_pOrthographicTexture`) rather than sharing one, since all
three stages get computed before any of them is displayed (§11) — a shared texture would leave
every panel showing whichever stage happened to render last.

## §11. `PipelineVisualizationWindow`: a second AppKit window, and its own `RasterRenderer`

`PipelineVisualizationWindow` is the secondary `NS::Window` that displays all four stages side by
side, in a 2×2 grid of labeled `ImageDisplayView`s (Chapter 10) it builds identically to how
`AppDelegate` builds the main window's three preview panes:

```cpp
// App/PipelineVisualizationWindow.cpp:37-88
PipelineVisualizationWindow::PipelineVisualizationWindow( MTL::Device* pDevice )
	: _pDevice( pDevice->retain() )
{
	using NS::StringEncoding::UTF8StringEncoding;

	gPipelineWindow = this;

	_pStageRenderer = new PipelineStageRenderer( _pDevice );
	_pViewportStageRenderer = new RasterRenderer( _pDevice );

	const CGRect windowFrame = ( CGRect ){ { 150.0, 120.0 }, { 970.0, 800.0 } };
	_pWindow = NS::Window::alloc()->init(
		windowFrame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "Pipeline Steps", UTF8StringEncoding ) );
	// See NSWindow.hpp's comment on this method: without it, closing this window via its title-bar
	// button would deallocate it, leaving this class's _pWindow (and gPipelineWindow) dangling.
	_pWindow->setReleasedWhenClosed( false );

	const CGRect contentFrame = ( CGRect ){ { 0.0, 0.0 }, windowFrame.size };
	NS::View*    pContentView = NS::View::alloc()->init( contentFrame );

	const char* titles[ 4 ] = { "1. Projective Matrix", "2. Camera Matrix", "3. Orthographic Matrix", "4. Viewport Matrix" };
	const CGRect panelFrames[ 4 ] = {
		( CGRect ){ { 10.0, 470.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
		( CGRect ){ { 480.0, 470.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
		( CGRect ){ { 10.0, 140.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
		( CGRect ){ { 480.0, 140.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
	};

	for ( int i = 0; i < 4; ++i )
	{
		CGRect labelFrame = panelFrames[ i ];
		labelFrame.origin.y += labelFrame.size.height + 2.0;
		labelFrame.size.height = 20.0;
		_pLabels[ i ] = makeLabel( labelFrame, titles[ i ] );
		pContentView->addSubview( _pLabels[ i ] );

		_pImageViews[ i ] = new ImageDisplayView( _pDevice, panelFrames[ i ] );
		pContentView->addSubview( _pImageViews[ i ]->view() );
	}

	_pRefreshButton = NS::Button::alloc()->init( ( CGRect ){ { 10.0, 15.0 }, { 110.0, 26.0 } } );
	_pRefreshButton->setTitle( NS::String::string( "Refresh", UTF8StringEncoding ) );
	_pRefreshButton->setTarget( _pRefreshButton );
	_pRefreshButton->setAction( NS::MenuItem::registerActionCallback( "pipelineRefreshClicked", onRefreshClicked ) );
	pContentView->addSubview( _pRefreshButton );

	_pWindow->setContentView( pContentView );
}
```

Two design choices here are easy to miss and both matter. First, this class constructs its *own*
`RasterRenderer` (`_pViewportStageRenderer`) rather than sharing `AppDelegate`'s main-window one —
the class comment states the reason directly: so a "Refresh" click in this window can never race a
"Render Raster"/"Compare" click on the main window over the same GPU buffers and texture. Second,
stages 1–3 render at a fixed `kDiagramWidth`×`kDiagramHeight` (460×300) regardless of the user's
width field, while stage 4 renders at the scene's own `params.width`/`height` — a direct reading of
the user's own wording, which asked for "the appropriate dimensions" for the Viewport-Matrix stage
specifically, and said nothing of the kind about the first three, which are schematic diagrams, not
"the render."

`showWithScene()` computes all four stages on a background thread — the same
`std::thread`-plus-`dispatch_async` pattern every render action in `AppDelegate` already
follows (Chapter 8) — then updates all four panels together and brings the window forward:

```cpp
// App/PipelineVisualizationWindow.cpp:109-130
void PipelineVisualizationWindow::showWithScene( const SceneDescription& scene )
{
	setControlsEnabled( false );

	std::thread( [ this, scene ]()
	{
		PipelineStageResult projective = _pStageRenderer->renderProjectiveStage( scene, kDiagramWidth, kDiagramHeight );
		PipelineStageResult camera = _pStageRenderer->renderCameraStage( scene, kDiagramWidth, kDiagramHeight );
		PipelineStageResult orthographic = _pStageRenderer->renderOrthographicStage( scene, kDiagramWidth, kDiagramHeight );
		RasterRenderResult  viewport = _pViewportStageRenderer->render( scene );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pImageViews[ 0 ]->displayTexture( projective.pTexture );
			_pImageViews[ 1 ]->displayTexture( camera.pTexture );
			_pImageViews[ 2 ]->displayTexture( orthographic.pTexture );
			_pImageViews[ 3 ]->displayTexture( viewport.pTexture );

			setControlsEnabled( true );
			_pWindow->makeKeyAndOrderFront( nullptr );
		} );
	} ).detach();
}
```

`onRefreshRequested` is a `std::function<void()>` the owner (`AppDelegate`) sets once, so this
window never needs to know about `ControlsPanel`'s existence — it just calls back out whenever its
own "Refresh" button is clicked, and the owner decides what "the current settings" mean.

## §12. `NS::Window::setReleasedWhenClosed()`: one more AppKit gap, filled the same way as always

This window is created lazily, once, on the first "Pipeline Steps" click, and kept alive for the
rest of the app's run — clicking the button again just re-shows the same window. That plan has a
sharp edge Cocoa creates by default: an `NSWindow` created programmatically has `releasedWhenClosed`
set to `YES` out of the box, meaning the user simply clicking the window's own title-bar close
button would deallocate the underlying Objective-C object, leaving `AppDelegate`'s
`PipelineVisualizationWindow*` (and this file's own `gPipelineWindow` trampoline pointer, §11)
dangling for whatever click came next. `metal-cpp-extensions`' vendored `NS::Window` wrapper had no
method for this at all, so one was added — the same `Object::sendMessage`-based pattern this file
already uses for its one prior local addition, `setAcceptsMouseMovedEvents()` (Chapter 10):

```cpp
// ThirdParty/metal-cpp-extensions/AppKit/NSWindow.hpp:52-58, 101-104
// Needed for the pipeline-visualization secondary window: Cocoa's default for a
// programmatically-created NSWindow is YES, meaning close() (including via the user
// clicking its title-bar close button) deallocates the underlying object — leaving
// AppDelegate's NS::Window* pointer dangling for a later re-show. Passing false keeps
// the C++ wrapper's pointer valid across close/reopen, the same way every other
// long-lived AppKit object in this app is managed explicitly.
void				setReleasedWhenClosed( bool releasedWhenClosed );

_NS_INLINE void NS::Window::setReleasedWhenClosed( bool releasedWhenClosed )
{
	Object::sendMessage< void >( this, sel_registerName( "setReleasedWhenClosed:" ), releasedWhenClosed );
}
```

Unlike the wholesale new files Chapter 10 covers (`NSButton.hpp`, `NSTextField.hpp`, `NSAlert.hpp`),
this is a small addition to an otherwise-vendored file that already had exactly one prior local
addition in it — so it gets an inline comment explaining the specific need, rather than the
file-header "Project-local addition" banner a wholly new file carries.

## Where this connects

- **Chapter 1** (`Core/ShaderTypes.h`, `Core/Scene.hpp/.cpp`) is where `CameraGPU`'s fields (§3, §7,
  §8) and the identity orientation basis every sphere uses (§2) come from.
- **Chapter 6** (`Export/EntityMesh.hpp/.cpp`) supplies pyramids' exact wireframe in §2, via the
  same `buildEntityMesh()` the exporters and Chapter 13's rasterizer both call.
- **Chapter 7** (`Export/SceneExporter.hpp/.cpp`) and its Unity-import investigation (recorded in
  CLAUDE.md, cited rather than re-derived) is where §4's framing-exclusion lesson was first learned.
- **Chapter 8** (`App/AppDelegate.hpp/.cpp`) is where the "Pipeline Steps" button's click handler
  lazily constructs the `PipelineVisualizationWindow` and wires `onRefreshRequested` (§11), and
  supplies the same background-thread-plus-`dispatch_async` pattern §11's `showWithScene()` follows.
- **Chapter 9** (`App/ControlsPanel.hpp/.cpp`) is where the "Pipeline Steps" button itself lives,
  on row 1 for the same "row 2 is already packed" reason `Load Scene` and `Render Raster` do.
- **Chapter 10** (`App/ImageDisplayView.hpp/.cpp`, `ThirdParty/metal-cpp-extensions`) is where every
  `ImageDisplayView` this window uses comes from unchanged, and where the AppKit-gap-filling
  precedent §12's `setReleasedWhenClosed()` follows was first established.
- **Chapter 11** (`RayTracerBenchTests/PipelineStageTests.cpp`) is the CPU-only regression coverage
  for this module's geometry math — the image-plane projection's fixed points, the framing-exclusion
  rule from §4, and the wireframe builder's vertex count — deliberately not a GPU pixel-comparison
  test, since these are hand-designed diagrams with no "expected pixels" to compare against.
- **Chapter 13** (`GPU/RasterRenderer.hpp/.cpp`, `GPU/CameraMath.hpp`, `Shaders/Raster.metal`) is
  where `CameraMath.hpp`'s view/projection primitives come from (§3, §8), and whose own renderer,
  reused unmodified through a second, independent instance (§11), *is* the fourth pipeline stage.
