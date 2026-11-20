// Road connectors ("edges"): the open ends of a road model where the next
// track piece attaches. Found purely from the geometry:
//  - cap loops: a closed, planar boundary loop (the road's cross-section at an
//    open end) whose neighbouring triangles all leave the plane on one side;
//  - strip ends: road surfaces built from open strips end in short straight
//    boundary pieces; those lying in one plane are merged into one connector.
// In the original courses most connectors of neighbouring pieces coincide.
#pragma once
#include "gma.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace fzgx
{
struct Connector
{
  glm::vec3 centre{0};
  glm::vec3 normal{0};    // points out of the piece
  glm::vec3 across{0};    // width direction (in the connector plane)
  float width = 0;
  std::vector<glm::vec3> lines;  // outline as line list (pairs of points)
};

std::vector<Connector> FindConnectors(const ModelMesh& mesh);

// Connector transformed by an object matrix.
Connector TransformConnector(const Connector& c, const glm::mat4& m);

// Two connectors that fit together: similar width, facing each other (or
// facing within `max_angle_cos`), centres within `range`.
bool ConnectorsFit(const Connector& a, const Connector& b, float max_facing_dot, float range);

// Rigid transform that moves connector `from` onto `to` (centres coincide,
// normals opposite, width directions aligned with the smaller rotation).
glm::mat4 ConnectTransform(const Connector& from, const Connector& to);
}  // namespace fzgx
