// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SPLAT_SRC_ANGLE_H
#define SPLAT_SRC_ANGLE_H

#include <assert.h>
#include <math.h>

#include "mathfu/matrix.h"
#include "mathfu/vector.h"
#include "mathfu/glsl_mappings.h"

#ifdef FPL_ANGLE_UNIT_TESTS
#include "gtest/gtest.h"
#endif // FPL_ANGLE_UNIT_TESTS

namespace fpl {

static const float kPi = static_cast<float>(M_PI);
static const float kTwoPi = static_cast<float>(2.0 * M_PI);
static const float kThreePi = static_cast<float>(3.0 * M_PI);
static const float kHalfPi = static_cast<float>(M_PI_2);
static const float kDegreesToRadians = static_cast<float>(M_PI / 180.0);
static const float kRadiansToDegrees = static_cast<float>(180.0 / M_PI);
static const float kMaxUniqueAngle = kPi;

// The biggest floating point number > -pi.
// That is, there are no floats x s.t. -pi < x < kMinUniqueAngle.
//
// Useful for clamping to the valid angle range:
//    [kMinUniqueAngle,kMaxUniqueAngle] == (-pi, pi]
// Note: "[" indicates an inclusive bound; "(" indicates exclusive.
//
// In general, after modding, we should clamp to the valid range.
// Floating point precision errors can generate numbers outside of
// (-pi, pi], even when the math is perfect.
static const float kMinUniqueAngle = -3.1415925f;


//    Purpose
//    =======
// Represents an angle in radians, uniquely in the range (-pi, pi].
//
// We include pi in the range, but exclude -pi, because pi and -pi
// are equivalent mod 2pi.
//
// Equivalence is key to this class. We want only one representation
// of every equivalent angle. For example, 0 and 2pi are both represented
// as 0, internally. This unique representation allows for comparison and
// precise arithmetic.
//
// Operators for + and - keep the angle values in the valid range.
//
//    Conversions to and from vectors and matrices
//    ============================================
// Conversions from and to XZ vectors follow the following convension:
// The vector is a point on unit circle corresponding to a sweep of 'angle'
// degrees from the x-axis towards the z-axis.
//   0     ==> ( 1,  0)
//   pi/2  ==> ( 0,  1)
//   pi    ==> (-1,  0)
//   3pi/2 ==> ( 0, -1)
//
//    Why use instead of Quaternions?
//    ===============================
// Quaternions are great for three dimensional rotations, but for many
// applications you only have two dimensional rotations. Instead of the
// four floats and heavy operations required by quaternions, an angle
// can represent the rotation in one float and reasonably light operations.
// Angles are easier to do trigonometry on. Also, they're conceptually
// simpler.
//
class Angle {
 public:
  Angle() : angle_(0.0f) {}

  // Create from 'angle', which is already in the valid range (-pi,pi].
  explicit Angle(float angle) : angle_(angle) { assert(IsValid()); }

  float angle() const { return angle_; }

  Angle Abs() const { return Angle(fabs(angle_)); }

  float ToDegrees() const {
    return kRadiansToDegrees * angle_;
  }

  mathfu::vec3 ToXZVector() const {
    float x, z;
    ToVector(&x, &z);
    return mathfu::vec3(x, 0.0f, z);
  }

  mathfu::mat3 ToXZRotationMatrix() const {
    float x, z;
    ToVector(&x, &z);
    return mathfu::mat3(   x, 0.0f,   -z,
                        0.0f, 1.0f, 0.0f,
                           z, 0.0f,    x);
  }

  Angle operator-() const {
    return Angle(ModIfNegativePi(-angle_));
  }

  // Check internal consistency. If class is functioning correctly, should
  // always return true.
  bool IsValid() const {
    return IsAngleInRange(angle_);
  }

  // Create from 'angle', which is in the range (-3pi,3pi].
  static Angle FromWithinThreePi(const float angle) {
    return Angle(ModWithinThreePi(angle));
  }

  // Create from 'degrees', which is in the range [-180, 180]
  static Angle FromDegrees(const float degrees) {
    return Angle(ModIfNegativePi(degrees * kDegreesToRadians));
  }

  // Create from the x,z coordinates of a vector, as described in the
  // comment at the head of the class.
  static Angle FromXZVector(const mathfu::vec3& v) {
    return Angle(ModIfNegativePi(atan2f(v[2], v[0])));
  }

  // Return true if 'angle' is within the valid range (-pi,pi], that is,
  // the range inclusive of +pi but exclusive of -pi.
  static bool IsAngleInRange(const float angle) {
    return kMinUniqueAngle <= angle && angle <= kMaxUniqueAngle;
  }

 private:
  friend Angle operator+(const Angle& a, const Angle& b);
  friend Angle operator-(const Angle& a, const Angle& b);
  friend bool operator==(const Angle& a, const Angle& b);
  friend bool operator!=(const Angle& a, const Angle& b);
#ifdef FPL_ANGLE_UNIT_TESTS
  FRIEND_TEST(AngleTests, RangeExtremes);
  FRIEND_TEST(AngleTests, IsAngleInRange);
  FRIEND_TEST(AngleTests, ModWithinThreePi);
  FRIEND_TEST(AngleTests, ModIfNegativePi);
#endif // FPL_ANGLE_UNIT_TESTS

  void ToVector(float* const x, float* const z) const {
    *x = cos(angle_);
    *z = sin(angle_);
  }

  // Take 'angle' in the range (-3pi,3pi] and return an equivalent angle in
  // the range (-pi,pi]. Note that angles are equivalent if they differ by 2pi.
  static float ModWithinThreePi(const float angle) {
    assert(-kThreePi < angle && angle < kThreePi);
    // These ternary operators should be converted into select statements by
    // the compiler.
    const float above = angle < kMinUniqueAngle ? angle + kTwoPi : angle;
    const float below = above > kMaxUniqueAngle ? above - kTwoPi : above;
    assert(IsAngleInRange(below));
    return below;
  }

  static float ModIfNegativePi(const float angle) {
    // Pi negates to -pi, which is outside the range so becomes +pi again.
    return angle < kMinUniqueAngle ? kMaxUniqueAngle : angle;
  }

  float angle_; // Angle in radians, in range (-pi, pi]
};

inline Angle operator+(const Angle& a, const Angle& b) {
  return Angle(Angle::ModWithinThreePi(a.angle_ + b.angle_));
}

inline Angle operator-(const Angle& a, const Angle& b) {
  return Angle(Angle::ModWithinThreePi(a.angle_ - b.angle_));
}

inline bool operator==(const Angle& a, const Angle& b) {
  return a.angle_ == b.angle_;
}

inline bool operator!=(const Angle& a, const Angle& b) {
  return !operator==(a, b);
}

}  // namespace fpl

#endif  // SPLAT_SRC_ANGLE_H