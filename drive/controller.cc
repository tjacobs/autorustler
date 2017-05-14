#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <iostream>

#include "drive/controller.h"
#include "drive/udplane.h"

using Eigen::Matrix2f;
using Eigen::Matrix3f;
using Eigen::MatrixXf;
using Eigen::Vector2f;
using Eigen::Vector3f;
using Eigen::VectorXf;
using Eigen::Lower;
using Eigen::SelfAdjointView;

static const uint8_t U_THRESH = 112;

static float MAX_THROTTLE = 1.0;

// static const float TRACTION_LIMIT = 3.4;  // maximum v*w product
// static const float kpy = 0.05;

static const float TRACTION_LIMIT = 2.5;  // maximum v*w product
static const float kpy = 0.05;

static const float kvy = 0.4;
// static const float kvy = 0.0;

static const float LANE_OFFSET = 0;


// EKF state order, 13-dimensional:
// ye, psie, w, v, k, Cv, Tv, Cs, Ts, mu_s, mu_g, mu_ax, mu_ay
// 2.35488   -3.35807   -1.25895  -0.711344   0.234494   0.134446 -0.0395876    1.09513

DriveController::DriveController(): x_(13), P_(13, 13) {
  ResetState();
}

void DriveController::ResetState() {
  // set up initial state
  x_ << 0, 0, 0, 0, 0,
     2.3, 0, -1.2, -0.7,  // Cv and Cs are w.r.t. 16-bit ints
     0.2, 0, 0, 0;
  P_.setZero();
  P_.diagonal() << 25., 1., 0.01, 0.01, 0.0001,
    1., 0.01, 0.01, 0.01,
    0.01, 0.0001, 0.0001, 0.0001;
}

void DriveController::PredictStep(
    float u_acceleration, float u_steering, float dt) {
  float ye = x_[0], psie = x_[1], w = x_[2], v = x_[3], k = x_[4];
  float Cv = x_[5], Tv = x_[6], Cs = x_[7], Ts = x_[8];
  float mu_s = x_[9], mu_g = x_[10], mu_ax = x_[11], mu_ay = x_[12];

  float eCv = exp(Cv), eTv = exp(Tv), eCs = exp(Cs), eTs = exp(Ts);

  x_[0] -= dt*v * sin(psie);
  x_[1] += dt*(w + v * k * cos(psie) / (1 - k * ye));
  x_[2] += dt*(v*eCs*(u_steering - mu_s) - w) / eTs;  // fixme: this should be clipped at some traction limit, tanh?
  x_[3] += dt*(eCv * u_acceleration - v) / eTv;

  // ... dear reader, i am sorry about what you see here,
  // but take heart: this was autogenerated from sympy.

  float kyem1 = k*ye - 1;

  MatrixXf Fk(13, 13);
  Fk <<
    1, -dt*v*cos(psie), 0, -dt*sin(psie), 0, 0, 0, 0, 0, 0, 0, 0, 0,

    dt*k*k*v*cos(psie)/(kyem1*kyem1), (dt*k*v*sin(psie) + kyem1)/kyem1,
    dt, -dt*k*cos(psie)/kyem1, dt*v*cos(psie)/(kyem1*kyem1), 0, 0, 0, 0, 0, 0, 0, 0,

    0, 0, -dt*exp(-Ts) + 1, -dt*(mu_s - u_steering)*exp(Cs - Ts), 0, 0, 0,
    -dt*v*(mu_s - u_steering)*exp(Cs - Ts), dt*(v*(mu_s - u_steering)*exp(Cs) + w)*exp(-Ts),
    -dt*v*exp(Cs - Ts), 0, 0, 0,

    0, 0, 0, -dt*exp(-Tv) + 1, 0, dt*u_acceleration*exp(Cv - Tv),
    dt*(-u_acceleration*exp(Cv) + v)*exp(-Tv), 0, 0, 0, 0, 0, 0,

    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1;

  // velocity fudge factor for determine covariance growth
  v = fabsf(x_[3]) + 0.5;
  VectorXf Qk(13);
  Qk <<
    v*dt*0.1,  // ye shouldn't jitter much at all
    v*dt*0.01,  // neither should psie
    v*dt*0.2,  // angular steering should mostly be correct from controls but give it slack
    dt*0.001,   // same with velocity
    v*dt*0.001,  // curvature can change nearly instantaneously, but its scale is tiny
    1e-4, 1e-4, 1e-4, 1e-4,  // the remainder are undetermined constants
    1e-4, 1e-4, 1e-4, 1e-4;

  P_ = Fk * P_ * Fk.transpose();
  P_.diagonal() += Qk;
}

void DriveController::UpdateCamera(const uint8_t *yuv) {
  // linear regression on yellow pixels in undistorted/reprojected ground plane
  Matrix2f regXTX = Matrix2f::Zero();
  Vector2f regXTy = Vector2f::Zero();
  double regyTy = 0;
  int regN = 0;

  // size_t ybufidx = 0;  // notused
  size_t bufidx = udplane_ytop*320;
  size_t udidx = 0;
  // size_t vbufidx = 640*480 + 320*240 + udplane_ytop*320;  // notused yet

  for (int y = 0; y < 240 - udplane_ytop; y++) {
    for (int x = 0; x < 320; x++, bufidx++, udidx++) {
      uint8_t u = yuv[640*480 + bufidx];
      // uint8_t v = yuv[640*480 + 320*240 + bufidx];
      if (u >= U_THRESH) continue;  // was 105
      // if (v <= 140) continue;
      
      float pu = udplane[udidx*2];
      float pv = udplane[udidx*2 + 1];

      // add x, y to linear regression
      Vector2f regX(1, pv);
      regXTX.noalias() += regX * regX.transpose();
      regXTy.noalias() += regX * pu;
      regyTy += pu * pu;
      regN += 1;
    }
  }

  // not enough data, don't even try to do an update
  if (regN < 20) {
    return;
  }

  Matrix2f XTXinv = regXTX.inverse();
  Vector2f B = XTXinv * regXTy;
  float r2 = B.transpose().dot(regXTX * B) - 2*B.dot(regXTy) + regyTy;
  // r2 /= regN;

#if 0
  std::cout << "XTX\n" << regXTX << "\n";
  std::cout << "XTy " << regXTy.transpose() << "\n";
  std::cout << "yTy " << regyTy << "\n";
  std::cout << "XTXinv\n" << XTXinv << "\n";
  std::cout << "B " << B << "\n";
  std::cout << "r2 " << r2 << "\n";
#endif

  Matrix2f Rk = XTXinv * r2;
  Rk(1, 1) += 0.01;  // slope is a bit iffy; add a bit of noise covariance

  // ok, we've obtained our linear fit B and our measurement covariance Rk
  // now do the sensor fusion step
  float ye = x_[0], psie = x_[1], w = x_[2], v = x_[3], k = x_[4];
  float Cv = x_[5], Tv = x_[6], Cs = x_[7], Ts = x_[8];
  float mu_s = x_[9], mu_g = x_[10], mu_ax = x_[11], mu_ay = x_[12];

  Vector2f y_k = B - Vector2f(ye / cos(psie), tan(psie));
  MatrixXf Hk(2, 13);
  float tan_psi = tan(psie);
  float sec_psi = 1.0/cos(psie);
  Hk <<
    sec_psi, ye*tan_psi*sec_psi, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

    0, sec_psi*sec_psi, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0;

  Matrix2f S = Hk * P_ * Hk.transpose() + Rk;
  MatrixXf K = P_ * Hk.transpose() * S.inverse();

#if 0
  std::cout << "mb_K\n" << K << "\n";
  std::cout << "y_k\n" << y_k.transpose()
    << " pred " << (y_k - B).transpose()
    << " meas " << B.transpose() << "\n";
#endif

  // finally, state update via kalman gain
  x_.noalias() += K * y_k;
  P_ = (MatrixXf::Identity(13, 13) - K*Hk) * P_;
}

void DriveController::UpdateIMU(
    const Vector3f &accel, const Vector3f &gyro,
    float u_acceleration) {
  float ye = x_[0], psie = x_[1], w = x_[2], v = x_[3], k = x_[4];
  float Cv = x_[5], Tv = x_[6], Cs = x_[7], Ts = x_[8];
  float mu_s = x_[9], mu_g = x_[10], mu_ax = x_[11], mu_ay = x_[12];

  Vector3f zk(
      w + mu_g,
      v*w + mu_ax,
      -(exp(Cv) * u_acceleration - v) / exp(Tv) + mu_ay);
  MatrixXf Hk(3, 13);
  Hk <<
    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,

    0, 0, v, w, 0, 0, 0, 0, 0, 0, 0, 1, 0,

    0, 0, 0, exp(-Tv), 0, -u_acceleration*exp(Cv - Tv),
      (u_acceleration*exp(Cv) - v)*exp(-Tv), 0, 0, 0, 0, 0, 1;

  const float g_conv = 980. * 2.364 / 20.;
  Vector3f yk = Vector3f(gyro[2], accel[0] * g_conv, accel[1] * g_conv) - zk;

  // accelerometer measurements are hugely noisy, almost not even worth it
  Vector3f Rk(1e-4, g_conv * g_conv / 16, g_conv * g_conv / 16);

  Matrix3f S = Hk * P_ * Hk.transpose();
  S.diagonal() += Rk;
  MatrixXf K = P_ * Hk.transpose() * S.inverse();

#if 0
  std::cout << "IMU_K\n" << K << "\n";
  std::cout << "y_k\n" << yk.transpose()
    << " pred " << zk.transpose()
    << " meas " << (yk + zk).transpose() << "\n";
#endif

  x_.noalias() += K * yk;
  P_ = (MatrixXf::Identity(13, 13) - K*Hk) * P_;
}

void DriveController::UpdateState(const uint8_t *yuv, size_t yuvlen,
      float throttle_in, float steering_in,
      const Vector3f &accel,
      const Vector3f &gyro, float dt) {

  if (isinf(x_[0]) || isnan(x_[0])) {
    fprintf(stderr, "WARNING: kalman filter diverged to inf/NaN! resetting!\n");
    ResetState();
  }

  PredictStep(throttle_in, steering_in, 1.0/30.0);
#if 0
  std::cout << "predict x" << x_.transpose() << std::endl;
  std::cout << "predict P" << P_.diagonal().transpose() << std::endl;
#endif
  if (yuvlen == 640*480 + 320*240*2) {
    UpdateCamera(yuv);
  } else {
    fprintf(stderr, "DriveController::UpdateState: invalid yuvlen %ld\n",
        yuvlen);
  }
#if 0
  std::cout << "camera x" << x_.transpose() << std::endl;
  std::cout << "camera P" << P_.diagonal().transpose() << std::endl;
#endif
  UpdateIMU(accel, gyro, throttle_in);

  if (x_[1] > M_PI/2) {
    x_[1] -= M_PI;
  } else if (x_[1] < -M_PI/2) {
    x_[1] += M_PI;
  }

  x_[3] = fabsf(x_[3]);  // (velocity) dumb hack: keep us from going backwards when we're confused
  x_[4] = fmax(fmin(x_[4], 0.3), -0.3);  // clip curvature so it doesn't go too extreme

  std::cout << "x " << x_.transpose() << std::endl;
  // std::cout << "P " << P_.diagonal().transpose() << std::endl;
}

bool DriveController::GetControl(float *throttle_out, float *steering_out) {
  float ye = x_[0], psie = x_[1], w = x_[2], v = x_[3], k = x_[4];
  float Cv = x_[5], Tv = x_[6], Cs = x_[7], Ts = x_[8];
  float mu_s = x_[9], mu_g = x_[10], mu_ax = x_[11], mu_ay = x_[12];
  float eCv = exp(Cv), eTv = exp(Tv), eCs = exp(Cs), eTs = exp(Ts);

  float cpsi = cos(psie), spsi = sin(psie);
  float dx = cpsi / (1.0 - k*ye);
  float sign = 1;  // should be sign(v*dx) but we're not handling backwards motion

#if 0
  if (fabs(k) >= 0.1) {
    // hard limit on tight curves
    MAX_THROTTLE = 0.4;
  }
#endif

  // Alain Micaelli, Claude Samson. Trajectory tracking for unicycle-type and
  // two-steering-wheels mobile robots. [Research Report] RR-2097, INRIA. 1993.
  // <inria-00074575>
  float w_target = v * dx * ((ye - LANE_OFFSET) * dx * kpy*cpsi + spsi*(k*spsi - kvy*cpsi*sign) - k);

  float cur_steer = w / v;
  printf("w_target %f w %f traction_limit %f v %f y %f psi %f steer %f\n",
      w_target, w, TRACTION_LIMIT, v, ye, psie, cur_steer);
  //  if (fabsf(w_target) < TRACTION_LIMIT) {
  // good news, we can floor it!
  // we could try to use eTs here to steer more aggressively, but it's not a
  // real great model
  // could do one-step lookahead on v, but nah
  *steering_out = fmin(fmax(mu_s + w_target / (v * eCs), -1), 1);

  //    *throttle_out = MAX_THROTTLE;
  //  } else {
  // as v decreases we should come back within the traction limit
  //    float sign = w_target > 0 ? 1 : -1;
  //    *steering_out = sign;
  // refine limited w target
  float delta = eCs * (*steering_out - mu_s);  // actual steering after limiting
  float v_target = fabsf(TRACTION_LIMIT / delta);
  // todo: more aggressive braking
  *throttle_out = fmin(fmax(v_target / eCv, -1), MAX_THROTTLE);

  printf("  throttle %f steer %f\n", *throttle_out, *steering_out);
  return true;
}

