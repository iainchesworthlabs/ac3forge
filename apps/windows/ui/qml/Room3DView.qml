import QtQuick
import QtQuick3D

import Ac3ForgeDesk

// The room in three dimensions: the listener at the centre, the reference
// speakers (5.1, 7.1 or 7.1.4, chosen in Settings; automatically 5.1 while
// the stream is bed only and 7.1.4 once objects are on), and every placed
// application as a sphere at its object position, a split pair as two.
//
// Interaction: drag an application to move it across the floor at its
// height; hold Shift to move it up and down instead. Drag empty space to
// orbit the camera (the room follows the mouse); the wheel zooms.
//
// Room coordinates (ac3::oba::Position: x 0 left to 1 right, y 0 front to
// 1 back, z -1 floor to +1 ceiling) map to scene units the way the
// engine's spatial sink maps them: 4 m wide, 4 m deep, ear level at 0.
Item {
    id: root
    property var apps: []
    property int selectedApp: -1
    property string caption: ""
    signal select(int app)
    signal moved(int app, double x, double y, double z)

    readonly property real roomWidth: 400
    readonly property real roomDepth: 400
    readonly property real roomHeight: 260
    readonly property real spread: 0.15
    function sceneX(x) { return (x - 0.5) * roomWidth; }
    function sceneZ(y) { return (y - 0.5) * roomDepth; }
    function sceneY(z) { return z * roomHeight / 2; }
    function roomX(sx) { return Math.max(0, Math.min(1, sx / roomWidth + 0.5)); }
    function roomY(sz) { return Math.max(0, Math.min(1, sz / roomDepth + 0.5)); }
    function roomZ(sy) { return Math.max(-1, Math.min(1, sy / (roomHeight / 2))); }

    // The reference layout: what Settings says, or by the stream's state.
    readonly property string layout: DeskController.roomLayout === "auto"
        ? (DeskController.objectsEnabled ? "7.1.4" : "5.1") : DeskController.roomLayout
    readonly property var speakers: {
        const base = [
            { name: "L", x: 0.0, y: 0.0, z: 0.0 }, { name: "R", x: 1.0, y: 0.0, z: 0.0 }, { name: "C", x: 0.5, y: 0.0, z: 0.0 }];
        if (layout === "5.1") {
            return base.concat([{ name: "Ls", x: 0.0, y: 1.0, z: 0.0 }, { name: "Rs", x: 1.0, y: 1.0, z: 0.0 }]);
        }
        const seven = base.concat([
            { name: "Ls", x: 0.0, y: 0.55, z: 0.0 }, { name: "Rs", x: 1.0, y: 0.55, z: 0.0 },
            { name: "Lrs", x: 0.0, y: 1.0, z: 0.0 }, { name: "Rrs", x: 1.0, y: 1.0, z: 0.0 }]);
        if (layout === "7.1") {
            return seven;
        }
        return seven.concat([
            { name: "Ltf", x: 0.2, y: 0.25, z: 1.0 }, { name: "Rtf", x: 0.8, y: 0.25, z: 1.0 },
            { name: "Ltr", x: 0.2, y: 0.75, z: 1.0 }, { name: "Rtr", x: 0.8, y: 0.75, z: 1.0 }]);
    }

    Text {
        id: heading
        text: root.caption + " · " + root.layout
        color: Theme.textMuted
        font.family: Theme.monoFamily
        font.pixelSize: 11
        font.letterSpacing: 0.6
    }
    Text {
        anchors.right: parent.right
        text: qsTr("drag an application to move it · right-drag or Shift for height · drag space to orbit · wheel to zoom")
        color: Theme.textMuted
        font.family: Theme.monoFamily
        font.pixelSize: 11
        elide: Text.ElideLeft
        width: Math.max(0, parent.width - heading.implicitWidth - 12)
        horizontalAlignment: Text.AlignRight
    }
    Rectangle {
        id: frame
        anchors.top: heading.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        color: Theme.neutral100
        border.color: Theme.divider
        border.width: 1
        View3D {
            id: view
            anchors.fill: parent
            anchors.margins: 1
            // Named explicitly: rendering finds a camera in the scene on its
            // own, but mapTo3DScene (the drag and drop rays) uses this
            // property and returned zero vectors while it was unset.
            camera: camera
            environment: SceneEnvironment {
                clearColor: Theme.neutral100
                backgroundMode: SceneEnvironment.Color
                antialiasingMode: SceneEnvironment.MSAA
                antialiasingQuality: SceneEnvironment.High
            }
            // The camera orbits the listener. The default is the view Iain set
            // by hand: from behind the rear wall, nearly level, looking down the
            // centre line with the whole width in frame.
            Node {
                id: orbit
                eulerRotation.x: -14
                eulerRotation.y: 0
                PerspectiveCamera {
                    id: camera
                    position: Qt.vector3d(0, 0, 560)
                    clipNear: 10
                    clipFar: 5000
                }
            }
            // A key light with soft shadows, so the objects and speakers sit
            // on the floor rather than float over it, and a low fill.
            DirectionalLight {
                eulerRotation.x: -55
                eulerRotation.y: 25
                brightness: 1.3
                ambientColor: Qt.rgba(0.30, 0.30, 0.34, 1)
                castsShadow: true
                shadowFactor: 55
                shadowMapQuality: Light.ShadowMapQualityHigh
                shadowBias: 24
                pcfFactor: 6
            }
            DirectionalLight {
                eulerRotation.x: -20
                eulerRotation.y: -120
                brightness: 0.35
            }
            // The floor, with a faint grid so distance and depth read; the
            // walls as an outline at ear level.
            Model {
                source: "#Cube"
                position: Qt.vector3d(0, -root.roomHeight / 2, 0)
                scale: Qt.vector3d(root.roomWidth / 100, 0.02, root.roomDepth / 100)
                receivesShadows: true
                materials: PrincipledMaterial {
                    roughness: 0.95
                    baseColorMap: Texture {
                        generateMipmaps: true
                        mipFilter: Texture.Linear
                        sourceItem: Canvas {
                            width: 512
                            height: 512
                            // Painted once per palette: a canvas does not
                            // repaint when a colour it read changes, so
                            // the floor kept the dark palette after a
                            // switch to light.
                            property color fill: Theme.neutral300
                            property color line: Theme.neutral400
                            onFillChanged: requestPaint()
                            onLineChanged: requestPaint()
                            onPaint: {
                                const c = getContext("2d");
                                c.fillStyle = fill;
                                c.fillRect(0, 0, width, height);
                                c.strokeStyle = line;
                                c.lineWidth = 1.5;
                                for (let i = 0; i <= 8; ++i) {
                                    const v = i * width / 8;
                                    c.beginPath(); c.moveTo(v, 0); c.lineTo(v, height); c.stroke();
                                    c.beginPath(); c.moveTo(0, v); c.lineTo(width, v); c.stroke();
                                }
                            }
                        }
                    }
                }
                pickable: false
            }
            // The walls as an outline at ear level, and the ceiling as a
            // fainter one at the height layer, so the top speakers hang from
            // something rather than float.
            Repeater3D {
                model: [
                    { x: 0, y: 0, z: -root.roomDepth / 2, sx: root.roomWidth / 100, sz: 0.02, top: false },
                    { x: 0, y: 0, z: root.roomDepth / 2, sx: root.roomWidth / 100, sz: 0.02, top: false },
                    { x: -root.roomWidth / 2, y: 0, z: 0, sx: 0.02, sz: root.roomDepth / 100, top: false },
                    { x: root.roomWidth / 2, y: 0, z: 0, sx: 0.02, sz: root.roomDepth / 100, top: false },
                    { x: 0, y: root.roomHeight / 2, z: -root.roomDepth / 2, sx: root.roomWidth / 100, sz: 0.02, top: true },
                    { x: 0, y: root.roomHeight / 2, z: root.roomDepth / 2, sx: root.roomWidth / 100, sz: 0.02, top: true },
                    { x: -root.roomWidth / 2, y: root.roomHeight / 2, z: 0, sx: 0.02, sz: root.roomDepth / 100, top: true },
                    { x: root.roomWidth / 2, y: root.roomHeight / 2, z: 0, sx: 0.02, sz: root.roomDepth / 100, top: true }
                ]
                delegate: Model {
                    required property var modelData
                    source: "#Cube"
                    visible: !modelData.top || root.layout === "7.1.4"
                    position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                    scale: Qt.vector3d(modelData.sx, 0.02, modelData.sz)
                    castsShadows: false
                    receivesShadows: false
                    materials: PrincipledMaterial {
                        baseColor: Theme.divider
                        lighting: PrincipledMaterial.NoLighting
                        opacity: modelData.top ? 0.45 : 1.0
                    }
                }
            }
            // The listener.
            Model {
                source: "#Sphere"
                scale: Qt.vector3d(0.14, 0.14, 0.14)
                materials: PrincipledMaterial { baseColor: Theme.text; roughness: 0.6 }
            }
            // The reference speakers: a cabinet with a woofer and a tweeter,
            // turned to face the listener, for the floor layer; a round
            // in-ceiling unit facing down for the heights. Built from
            // primitives so every speaker matches; a glTF model could stand
            // in for either later.
            component CabinetSpeaker: Node {
                Model {  // the cabinet
                    source: "#Cube"
                    scale: Qt.vector3d(0.16, 0.24, 0.14)
                    castsShadows: true
                    materials: PrincipledMaterial { baseColor: Theme.neutral600; roughness: 0.75; metalness: 0.05 }
                }
                Model {  // the woofer, on the front face
                    source: "#Cylinder"
                    position: Qt.vector3d(0, -4, 7.2)
                    eulerRotation.x: 90
                    scale: Qt.vector3d(0.11, 0.008, 0.11)
                    materials: PrincipledMaterial { baseColor: Theme.neutral300; roughness: 0.9 }
                }
                Model {  // the dust cap
                    source: "#Sphere"
                    position: Qt.vector3d(0, -4, 7.6)
                    scale: Qt.vector3d(0.035, 0.035, 0.012)
                    materials: PrincipledMaterial { baseColor: Theme.neutral500; roughness: 0.5 }
                }
                Model {  // the tweeter
                    source: "#Cylinder"
                    position: Qt.vector3d(0, 7.5, 7.2)
                    eulerRotation.x: 90
                    scale: Qt.vector3d(0.045, 0.006, 0.045)
                    materials: PrincipledMaterial { baseColor: Theme.neutral400; roughness: 0.6; metalness: 0.2 }
                }
            }
            // The height units are surface-mounted cans below the ceiling
            // plane rather than flush discs: seen nearly level from behind
            // the room a disc is a line, and in mid-tones that read on both
            // palettes (the dark palette's low neutrals vanished into the
            // background).
            component CeilingSpeaker: Node {
                Model {  // the can, hanging from the ceiling plane
                    source: "#Cylinder"
                    position: Qt.vector3d(0, -5, 0)
                    scale: Qt.vector3d(0.2, 0.1, 0.2)
                    castsShadows: true
                    materials: PrincipledMaterial { baseColor: Theme.neutral600; roughness: 0.7; metalness: 0.05 }
                }
                Model {  // the trim ring at the can's mouth
                    source: "#Cylinder"
                    position: Qt.vector3d(0, -10.2, 0)
                    scale: Qt.vector3d(0.23, 0.014, 0.23)
                    materials: PrincipledMaterial { baseColor: Theme.neutral700; roughness: 0.5; metalness: 0.15 }
                }
                Model {  // the grille, facing down
                    source: "#Cylinder"
                    position: Qt.vector3d(0, -11.2, 0)
                    scale: Qt.vector3d(0.17, 0.006, 0.17)
                    materials: PrincipledMaterial { baseColor: Theme.neutral400; roughness: 0.95 }
                }
            }
            Repeater3D {
                model: root.speakers
                delegate: Node {
                    required property var modelData
                    readonly property real sx: root.sceneX(modelData.x)
                    readonly property real sz: root.sceneZ(modelData.y)
                    position: Qt.vector3d(sx, root.sceneY(modelData.z), sz)
                    // The unit, turned to face the listener at the origin.
                    // The label sits beside it, not inside it, so the
                    // turn does not carry into the label's own rotation.
                    Node {
                        eulerRotation.y: Math.atan2(-sx, -sz) * 180 / Math.PI
                        Loader3D {
                            sourceComponent: modelData.z > 0 ? ceiling : cabinet
                            Component { id: cabinet; CabinetSpeaker {} }
                            Component { id: ceiling; CeilingSpeaker {} }
                        }
                    }
                    // The name on a pill that always faces the camera (the
                    // orbit's rotation is the camera's): a label turned with
                    // its speaker read mirrored from behind and edge-on from
                    // the side. Height labels carry the accent so the layer
                    // reads at a glance.
                    Node {
                        y: modelData.z > 0 ? (modelData.y < 0.5 ? -28 : 18) : 24
                        rotation: orbit.rotation
                        readonly property real distance: Qt.vector3d(sx, root.sceneY(modelData.z), sz).minus(camera.scenePosition).length()
                        readonly property real k: Math.max(0.5, Math.min(2.0, distance / 560))
                        scale: Qt.vector3d(k, k, k)
                        Rectangle {
                            anchors.centerIn: parent
                            width: label.implicitWidth + 12
                            height: label.implicitHeight + 4
                            radius: height / 2
                            color: modelData.z > 0 ? Theme.accent100 : Theme.surface
                            border.color: modelData.z > 0 ? Theme.accent400 : Theme.divider
                            border.width: 1
                            Text {
                                id: label
                                anchors.centerIn: parent
                                text: modelData.name
                                color: modelData.z > 0 ? Theme.accent700 : Theme.text
                                font.family: Theme.monoFamily
                                font.pixelSize: 18
                            }
                        }
                    }
                }
            }
            // Placed applications, one sphere per object. The sphere carries
            // the application id so a pick can name it.
            Repeater3D {
                model: root.apps
                delegate: Node {
                    id: appNode
                    required property var modelData
                    readonly property var app: modelData
                    readonly property bool placed: app.slot >= 0
                    readonly property bool selected: app.app === root.selectedApp
                    visible: placed
                    Repeater3D {
                        model: appNode.app.width === 2 ? [0, 1] : [-1]
                        delegate: Node {
                            required property int modelData
                            readonly property int side: modelData  // -1 mono, 0 left, 1 right
                            readonly property real ox: side < 0 ? appNode.app.x : (side === 0 ? appNode.app.lx : appNode.app.rx)
                            readonly property real oy: side < 0 ? appNode.app.y : (side === 0 ? appNode.app.ly : appNode.app.ry)
                            readonly property real oz: side < 0 ? appNode.app.z : (side === 0 ? appNode.app.lz : appNode.app.rz)
                            position: Qt.vector3d(root.sceneX(ox), root.sceneY(oz), root.sceneZ(oy))
                            // The object is a card carrying the application's own
                            // icon (or its monogram), turned to face the camera: the
                            // orbit node's rotation is the camera's, and a
                            // rectangle faces its own +z, so a card rotated with
                            // the orbit always faces the viewer.
                            Node {
                                rotation: orbit.rotation
                                Model {
                                    source: "#Rectangle"
                                    scale: Qt.vector3d(0.42, 0.42, 1)
                                    pickable: true
                                    castsShadows: true
                                    property int appId: appNode.app.app
                                    materials: PrincipledMaterial {
                                        lighting: PrincipledMaterial.NoLighting
                                        alphaMode: PrincipledMaterial.Blend
                                        baseColorMap: Texture {
                                            generateMipmaps: true
                                            mipFilter: Texture.Linear
                                            magFilter: Texture.Linear
                                            sourceItem: Item {
                                                width: 192
                                                height: 192
                                                Rectangle {
                                                    anchors.fill: parent
                                                    color: "transparent"
                                                    border.color: appNode.selected ? Theme.accent : "transparent"
                                                    border.width: 10
                                                }
                                                AppIcon {
                                                    anchors.centerIn: parent
                                                    size: 160
                                                    name: appNode.app.name
                                                    imagePath: appNode.app.imagePath
                                                    dimmed: appNode.app.silent
                                                    fill: appNode.selected ? Theme.accent600 : Theme.neutral700
                                                }
                                                Text {
                                                    visible: side >= 0
                                                    anchors.right: parent.right
                                                    anchors.bottom: parent.bottom
                                                    anchors.margins: 4
                                                    text: side === 0 ? "L" : "R"
                                                    color: Theme.text
                                                    font.family: Theme.monoFamily
                                                    font.pixelSize: 34
                                                    font.bold: true
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            // Height at a glance: a stem from the card's edge to a
                            // foot on the ear-level plane, on the near side of the
                            // card for a raised object and the far side for a
                            // lowered one, never through the icon.
                            readonly property real cardHalf: 21
                            readonly property real stemLength: Math.max(0, Math.abs(root.sceneY(oz)) - cardHalf)
                            Model {
                                source: "#Cylinder"
                                visible: stemLength > 2
                                position: Qt.vector3d(0, oz > 0 ? -(cardHalf + stemLength / 2) : (cardHalf + stemLength / 2), 0)
                                scale: Qt.vector3d(0.012, stemLength / 100, 0.012)
                                materials: PrincipledMaterial { baseColor: Theme.accent400; lighting: PrincipledMaterial.NoLighting }
                            }
                            Model {
                                source: "#Cylinder"
                                visible: Math.abs(oz) > 0.02
                                position: Qt.vector3d(0, -root.sceneY(oz), 0)
                                scale: Qt.vector3d(0.08, 0.006, 0.08)
                                materials: PrincipledMaterial { baseColor: Theme.accent400; lighting: PrincipledMaterial.NoLighting; opacity: 0.8 }
                            }
                        }
                    }
                    Node {
                        visible: appNode.selected
                        position: Qt.vector3d(root.sceneX(appNode.app.x), root.sceneY(appNode.app.z) + 34, root.sceneZ(appNode.app.y))
                        Text {
                            anchors.centerIn: parent
                            text: appNode.app.name
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 20
                        }
                    }
                }
            }
        }

        DropArea {
            anchors.fill: parent
            keys: ["app"]
            onDropped: function(drop) {
                const source = drop.source;
                if (!(source && source.app)) return;
                // The point on the ear-level plane under the drop.
                const near = view.mapTo3DScene(Qt.vector3d(drop.x - 1, drop.y - 1, 0.2));
                const far = view.mapTo3DScene(Qt.vector3d(drop.x - 1, drop.y - 1, 0.8));
                const dir = far.minus(near);
                if (Math.abs(dir.y) < 1e-6) return;
                const t = -near.y / dir.y;
                if (t < 0) return;
                const p = near.plus(dir.times(t));
                root.moved(source.app.app, root.roomX(p.x), root.roomY(p.z), 0);
                root.select(source.app.app);
                drop.accept(Qt.MoveAction);
            }
        }
        MouseArea {
            id: mouse
            anchors.fill: parent
            property real lastX: 0
            property real lastY: 0
            // The application under the press, if any, and how far the
            // pick point sat from its centre (so it does not jump).
            property var dragApp: null
            property bool dragHeight: false
            property real dragOffsetX: 0
            property real dragOffsetY: 0
            property real dragOffsetZ: 0
            cursorShape: dragApp ? (dragHeight ? Qt.SizeVerCursor : Qt.SizeAllCursor) : Qt.ArrowCursor
            preventStealing: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton

            // A point in the scene where the ray through (px, py) meets the
            // horizontal plane at height `planeY`, or null when the ray runs
            // away from it. Two unprojections make the ray; the depth is a
            // normalised 0..1 (mapTo3DScene returns a zero vector outside
            // that range, and the ends are degenerate), so two points
            // partway along.
            function planeHit(px, py, planeY) {
                const near = view.mapTo3DScene(Qt.vector3d(px, py, 0.2));
                const far = view.mapTo3DScene(Qt.vector3d(px, py, 0.8));
                const dir = far.minus(near);
                if (Math.abs(dir.y) < 1e-6) return null;
                const t = (planeY - near.y) / dir.y;
                if (t < 0) return null;
                return near.plus(dir.times(t));
            }
            // For height drags: the point where the ray meets the vertical
            // plane facing the camera through the object's floor position.
            function verticalHit(px, py, atX, atZ) {
                const near = view.mapTo3DScene(Qt.vector3d(px, py, 0.2));
                const far = view.mapTo3DScene(Qt.vector3d(px, py, 0.8));
                const dir = far.minus(near);
                // Plane normal: the camera's forward projected onto the floor.
                const fwd = camera.forward;
                const nx = fwd.x, nz = fwd.z;
                const denom = dir.x * nx + dir.z * nz;
                if (Math.abs(denom) < 1e-6) return null;
                const t = ((atX - near.x) * nx + (atZ - near.z) * nz) / denom;
                if (t < 0) return null;
                return near.plus(dir.times(t));
            }
            function appById(id) {
                for (let i = 0; i < root.apps.length; ++i) if (root.apps[i].app === id) return root.apps[i];
                return null;
            }

            onPressed: function(event) {
                lastX = event.x; lastY = event.y;
                dragApp = null;
                const hit = view.pick(event.x, event.y);
                if (hit.objectHit && hit.objectHit.appId !== undefined) {
                    const app = appById(hit.objectHit.appId);
                    if (app && !app.fullscreen) {
                        dragApp = app;
                        // Height: Shift with the left button, or the right button.
                        dragHeight = (event.modifiers & Qt.ShiftModifier) !== 0 || (event.button === Qt.RightButton);
                        root.select(app.app);
                        if (dragHeight) {
                            const p = verticalHit(event.x, event.y, root.sceneX(app.x), root.sceneZ(app.y));
                            dragOffsetZ = p ? app.z - root.roomZ(p.y) : 0;
                        } else {
                            const p = planeHit(event.x, event.y, root.sceneY(app.z));
                            dragOffsetX = p ? app.x - root.roomX(p.x) : 0;
                            dragOffsetY = p ? app.y - root.roomY(p.z) : 0;
                        }
                    }
                }
            }
            onPositionChanged: function(event) {
                if (!(event.buttons & (Qt.LeftButton | Qt.RightButton))) return;
                if (dragApp) {
                    if (dragHeight) {
                        const p = verticalHit(event.x, event.y, root.sceneX(dragApp.x), root.sceneZ(dragApp.y));
                        if (p) root.moved(dragApp.app, dragApp.x, dragApp.y, Math.max(-1, Math.min(1, root.roomZ(p.y) + dragOffsetZ)));
                    } else {
                        const p = planeHit(event.x, event.y, root.sceneY(dragApp.z));
                        if (p) root.moved(dragApp.app,
                                          Math.max(0, Math.min(1, root.roomX(p.x) + dragOffsetX)),
                                          Math.max(0, Math.min(1, root.roomY(p.z) + dragOffsetY)),
                                          dragApp.z);
                    }
                } else {
                    // Orbit: the room turns with the mouse.
                    orbit.eulerRotation.y -= (event.x - lastX) * 0.4;
                    orbit.eulerRotation.x = Math.max(-85, Math.min(10, orbit.eulerRotation.x - (event.y - lastY) * 0.3));
                }
                lastX = event.x; lastY = event.y;
            }
            onReleased: dragApp = null
            onCanceled: dragApp = null
            onWheel: function(wheel) {
                camera.position.z = Math.max(240, Math.min(2000, camera.position.z - wheel.angleDelta.y * 0.6));
            }
        }
    }
}
