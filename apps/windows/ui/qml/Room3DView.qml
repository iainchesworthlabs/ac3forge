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
        text: qsTr("drag an application to move it · Shift for height · drag space to orbit · wheel to zoom")
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
            environment: SceneEnvironment {
                clearColor: Theme.neutral100
                backgroundMode: SceneEnvironment.Color
                antialiasingMode: SceneEnvironment.MSAA
                antialiasingQuality: SceneEnvironment.Medium
            }
            // The camera orbits the listener. Framed so the room fills the
            // viewport at the default distance.
            Node {
                id: orbit
                eulerRotation.x: -28
                eulerRotation.y: 32
                PerspectiveCamera {
                    id: camera
                    position: Qt.vector3d(0, 0, 520)
                    clipNear: 10
                    clipFar: 5000
                }
            }
            DirectionalLight {
                eulerRotation.x: -45
                eulerRotation.y: 30
                brightness: 1.2
                ambientColor: Qt.rgba(0.35, 0.35, 0.38, 1)
            }
            // Floor as a thin slab; the walls as an outline at ear level.
            Model {
                source: "#Cube"
                position: Qt.vector3d(0, -root.roomHeight / 2, 0)
                scale: Qt.vector3d(root.roomWidth / 100, 0.02, root.roomDepth / 100)
                materials: PrincipledMaterial { baseColor: Theme.neutral300; roughness: 0.9 }
                pickable: false
            }
            Repeater3D {
                model: [
                    { x: 0, z: -root.roomDepth / 2, sx: root.roomWidth / 100, sz: 0.02 },
                    { x: 0, z: root.roomDepth / 2, sx: root.roomWidth / 100, sz: 0.02 },
                    { x: -root.roomWidth / 2, z: 0, sx: 0.02, sz: root.roomDepth / 100 },
                    { x: root.roomWidth / 2, z: 0, sx: 0.02, sz: root.roomDepth / 100 }
                ]
                delegate: Model {
                    required property var modelData
                    source: "#Cube"
                    position: Qt.vector3d(modelData.x, 0, modelData.z)
                    scale: Qt.vector3d(modelData.sx, 0.02, modelData.sz)
                    materials: PrincipledMaterial { baseColor: Theme.divider; lighting: PrincipledMaterial.NoLighting }
                }
            }
            // The listener.
            Model {
                source: "#Sphere"
                scale: Qt.vector3d(0.14, 0.14, 0.14)
                materials: PrincipledMaterial { baseColor: Theme.text; roughness: 0.6 }
            }
            // The reference speakers.
            Repeater3D {
                model: root.speakers
                delegate: Node {
                    required property var modelData
                    position: Qt.vector3d(root.sceneX(modelData.x), root.sceneY(modelData.z), root.sceneZ(modelData.y))
                    Model {
                        source: "#Cube"
                        scale: Qt.vector3d(0.16, 0.16, 0.16)
                        materials: PrincipledMaterial { baseColor: modelData.z > 0 ? Theme.neutral600 : Theme.neutral500; roughness: 0.8 }
                    }
                    Node {
                        y: 22
                        Text {
                            anchors.centerIn: parent
                            text: modelData.name
                            color: Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: 24
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
                        model: appNode.app.width === 2 ? [-root.spread, root.spread] : [0]
                        delegate: Node {
                            required property real modelData
                            position: Qt.vector3d(root.sceneX(Math.max(0, Math.min(1, appNode.app.x + modelData))),
                                                  root.sceneY(appNode.app.z),
                                                  root.sceneZ(appNode.app.y))
                            Model {
                                source: "#Sphere"
                                scale: Qt.vector3d(0.24, 0.24, 0.24)
                                pickable: true
                                property int appId: appNode.app.app
                                materials: PrincipledMaterial {
                                    baseColor: appNode.selected ? Theme.accent : Theme.accent700
                                    roughness: 0.5
                                }
                            }
                            // A stem down to ear level shows height at a glance.
                            Model {
                                source: "#Cylinder"
                                visible: Math.abs(appNode.app.z) > 0.02
                                position: Qt.vector3d(0, -root.sceneY(appNode.app.z) / 2, 0)
                                scale: Qt.vector3d(0.015, Math.abs(root.sceneY(appNode.app.z)) / 100, 0.015)
                                materials: PrincipledMaterial { baseColor: Theme.accent400; lighting: PrincipledMaterial.NoLighting }
                            }
                        }
                    }
                    Node {
                        position: Qt.vector3d(root.sceneX(appNode.app.x), root.sceneY(appNode.app.z) + 30, root.sceneZ(appNode.app.y))
                        Text {
                            anchors.centerIn: parent
                            text: appNode.app.name
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 26
                        }
                    }
                }
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
            cursorShape: dragApp ? Qt.SizeAllCursor : Qt.ArrowCursor
            preventStealing: true

            // A point in the scene where the ray through (px, py) meets the
            // horizontal plane at height `planeY`, or null when the ray runs
            // away from it. Two unprojections make the ray.
            function planeHit(px, py, planeY) {
                const near = view.mapTo3DScene(Qt.vector3d(px, py, 0));
                const far = view.mapTo3DScene(Qt.vector3d(px, py, 1));
                const dir = far.minus(near);
                if (Math.abs(dir.y) < 1e-6) return null;
                const t = (planeY - near.y) / dir.y;
                if (t < 0) return null;
                return near.plus(dir.times(t));
            }
            // For height drags: the point where the ray meets the vertical
            // plane facing the camera through the object's floor position.
            function verticalHit(px, py, atX, atZ) {
                const near = view.mapTo3DScene(Qt.vector3d(px, py, 0));
                const far = view.mapTo3DScene(Qt.vector3d(px, py, 1));
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
                        dragHeight = (event.modifiers & Qt.ShiftModifier) !== 0;
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
                if (!(event.buttons & Qt.LeftButton)) return;
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
                camera.position.z = Math.max(280, Math.min(2000, camera.position.z - wheel.angleDelta.y * 0.6));
            }
        }
    }
}
