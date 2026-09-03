import QtQuick
import QtQuick3D

import Ac3ForgeDesk

// The room in three dimensions: the listener at the centre, the five bed
// speakers where the bed slots are pinned, and every placed application as
// a sphere at its object position, a split pair as two. Display only: the
// plan and elevation views are where placement happens, this one is the
// picture of the result. Drag orbits the camera; the wheel zooms.
//
// Room coordinates (ac3::oba::Position: x 0 left to 1 right, y 0 front to
// 1 back, z -1 floor to +1 ceiling) map to scene metres the way the
// engine's spatial sink maps them: 4 m wide, 4 m deep, ear level at 0.
Item {
    id: root
    property var apps: []
    property int selectedApp: -1
    property string caption: ""
    signal select(int app)

    readonly property real roomWidth: 400
    readonly property real roomDepth: 400
    readonly property real roomHeight: 260
    readonly property real spread: 0.15

    function sceneX(x) { return (x - 0.5) * roomWidth; }
    function sceneZ(y) { return (y - 0.5) * roomDepth; }
    function sceneY(z) { return z * roomHeight / 2; }

    Text {
        id: heading
        text: root.caption
        color: Theme.textMuted
        font.family: Theme.monoFamily
        font.pixelSize: 11
        font.letterSpacing: 0.6
    }
    Text {
        anchors.right: parent.right
        text: qsTr("drag to orbit · wheel to zoom")
        color: Theme.textMuted
        font.family: Theme.monoFamily
        font.pixelSize: 11
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

            // The camera orbits the listener.
            Node {
                id: orbit
                eulerRotation.x: -26
                eulerRotation.y: 32
                PerspectiveCamera {
                    id: camera
                    position: Qt.vector3d(0, 0, 620)
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

            // Floor and ceiling as thin slabs; the walls as an outline.
            Model {
                source: "#Cube"
                position: Qt.vector3d(0, -root.roomHeight / 2, 0)
                scale: Qt.vector3d(root.roomWidth / 100, 0.02, root.roomDepth / 100)
                materials: PrincipledMaterial { baseColor: Theme.neutral300; roughness: 0.9 }
            }
            Repeater3D {
                // Four wall edges at ear level, as thin bars.
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

            // The bed's five speakers, where slots.hpp pins them.
            Repeater3D {
                model: [
                    { name: "L", x: 0.0, y: 0.0 }, { name: "R", x: 1.0, y: 0.0 }, { name: "C", x: 0.5, y: 0.0 },
                    { name: "Ls", x: 0.0, y: 1.0 }, { name: "Rs", x: 1.0, y: 1.0 }
                ]
                delegate: Node {
                    required property var modelData
                    position: Qt.vector3d(root.sceneX(modelData.x), 0, root.sceneZ(modelData.y))
                    Model {
                        source: "#Cube"
                        scale: Qt.vector3d(0.16, 0.16, 0.16)
                        materials: PrincipledMaterial { baseColor: Theme.neutral500; roughness: 0.8 }
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

            // Placed applications, one sphere per object.
            Repeater3D {
                model: root.apps
                delegate: Node {
                    id: appNode
                    required property var modelData
                    readonly property bool placed: modelData.slot >= 0
                    readonly property bool selected: modelData.app === root.selectedApp
                    visible: placed
                    Repeater3D {
                        model: appNode.modelData.width === 2 ? [-root.spread, root.spread] : [0]
                        delegate: Node {
                            required property real modelData
                            position: Qt.vector3d(root.sceneX(Math.max(0, Math.min(1, appNode.modelData.x + modelData))),
                                                  root.sceneY(appNode.modelData.z),
                                                  root.sceneZ(appNode.modelData.y))
                            Model {
                                source: "#Sphere"
                                scale: Qt.vector3d(0.22, 0.22, 0.22)
                                materials: PrincipledMaterial {
                                    baseColor: appNode.selected ? Theme.accent : Theme.accent700
                                    roughness: 0.5
                                }
                            }
                            // A stem down to ear level shows height at a glance.
                            Model {
                                source: "#Cylinder"
                                visible: Math.abs(appNode.modelData.z) > 0.02
                                position: Qt.vector3d(0, -root.sceneY(appNode.modelData.z) / 2, 0)
                                scale: Qt.vector3d(0.015, Math.abs(root.sceneY(appNode.modelData.z)) / 100, 0.015)
                                materials: PrincipledMaterial { baseColor: Theme.accent400; lighting: PrincipledMaterial.NoLighting }
                            }
                        }
                    }
                    Node {
                        position: Qt.vector3d(root.sceneX(appNode.modelData.x), root.sceneY(appNode.modelData.z) + 30, root.sceneZ(appNode.modelData.y))
                        Text {
                            anchors.centerIn: parent
                            text: appNode.modelData.name
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 26
                        }
                    }
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            property real lastX: 0
            property real lastY: 0
            onPressed: function(mouse) { lastX = mouse.x; lastY = mouse.y; }
            onPositionChanged: function(mouse) {
                if (!(mouse.buttons & Qt.LeftButton)) return;
                orbit.eulerRotation.y += (mouse.x - lastX) * 0.4;
                orbit.eulerRotation.x = Math.max(-85, Math.min(10, orbit.eulerRotation.x - (mouse.y - lastY) * 0.3));
                lastX = mouse.x; lastY = mouse.y;
            }
            onWheel: function(wheel) {
                camera.position.z = Math.max(350, Math.min(2000, camera.position.z - wheel.angleDelta.y * 0.6));
            }
        }
    }
}
