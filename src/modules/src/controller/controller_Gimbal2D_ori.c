/**
 * Authored by Mike Wen, 2024.March
 *
 * ============================================================================
 */

#include "controller_Gimbal2D.h"
#include "log.h"
#include "param.h"
#include "num.h"
#include "math3d.h"
#include "physicalConstants.h"

#include "stabilizer.h"
#include "stabilizer_types.h"
#include "debug.h"
#include "motors.h"

#define MOTOR_MAX_THRUST_N        (0.1472f)

// static struct mat33 CRAZYFLIE_INERTIA =
//     {{{16.6e-6f, 0.83e-6f, 0.72e-6f},
//       {0.83e-6f, 16.6e-6f, 1.8e-6f},
//       {0.72e-6f, 1.8e-6f, 29.3e-6f}}};

static bool isInit = false;

Gimbal2D_P_Type Gimbal2D_P = {
  .Kp = 1.0f,
  .ThrustUpperBound = 4.0f*MOTOR_MAX_THRUST_N,
  .ThrustLowerBound = 0.0f,
  //// Modified by Charles (Preset parameters, flashed into the hardware)
  .K = { { 1000.0f, 0.0f, 109.5f, 0.0f }, { 0.0f, 1000.0f, 0.0f, 109.5f } }, // { { 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f } }  Optimal Gain Matrix (default): 
  .B_inv = { { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } }, //  { { 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f } } Pseudo-inverse Matrix (default): 
  .J = {1.2e-5f, 1.2e-5f, 2.2e-5f}, // Moment of Inertia: Jx Jy Jz (default): {0.0f, 0.0f, 0.0f}
};

/**Instance of Input structure*/
Gimbal2D_U_Type Gimbal2D_U = {
        .qw_Base = 0.0,
        .qx_Base = 0.0,
        .qy_Base = 0.0,
        .qz_Base = 0.0,
        .index = 0.0,
        .thrust = 0.0, 
        .LastThrust = 0.0,
        .ClampedThrust = 0.0,
        .alpha_desired = 0.0,
        .beta_desired = 0.0,
        .qw_IMU = 0.0,
        .qx_IMU = 0.0,
        .qy_IMU = 0.0,
        .qz_IMU = 0.0,
        .omega_x = 0.0,
        .beta_speed = 0.0,
        .omega_z = 0.0,
        };

/**Instance of Output structure*/
Gimbal2D_Y_Type Gimbal2D_Y = {
        .IsClamped = 0,
        .Treset = 0,
        .m1 = 0,
        .m2 = 0,
        .m3 = 0,
        .m4 = 0,
        .acount_prev = 0.0,
        .bcount_prev = 0.0,
        .alpha_prev = 0.0,
        .beta_prev = 0.0,
        .alpha_e = 0.0,
        .beta_e = 0.0,
        .alpha_speed_e = 0.0,
        .beta_speed_e = 0.0,
        .error_alpha = 0.0,
        .error_beta = 0.0,
        .u_alpha = 0.0,
        .u_beta = 0.0,
        .t_m1 = 0.0,
        .t_m2 = 0.0,
        .t_m3 = 0.0,
        .t_m4 = 0.0,
        .error_alphas = 0.0,
        .error_betas = 0.0,
        .rollPart = 0.0,
        .pitchPart = 0.0,
        .yawPart = 0.0,
        .thrustPart = 0.0,
        .Tau_x = 0.0, 
        .Tau_y = 0.0,
        .Tau_z = 0.0,
        };

static void Gimbal2D_quatmultiply(const float q[4], const float r[4],
  float qout[4])
{
  qout[0] = ((q[0] * r[0] - q[1] * r[1]) - q[2] * r[2]) - q[3] * r[3];
  qout[1] = (q[0] * r[1] + r[0] * q[1]) + (q[2] * r[3] - q[3] * r[2]);
  qout[2] = (q[0] * r[2] + r[0] * q[2]) + (q[3] * r[1] - q[1] * r[3]);
  qout[3] = (q[0] * r[3] + r[0] * q[3]) + (q[1] * r[2] - q[2] * r[1]);
}

void Gimbal2D_quatToDCM(float *quat, struct mat33 *RotM)
{
  float q0 = *quat;
  float q1 = *(quat+1);
  float q2 = *(quat+2);
  float q3 = *(quat+3);

  RotM->m[0][0] = q0*q0+q1*q1-q2*q2-q3*q3;
  RotM->m[0][1] = 2.0F*(q1*q2-q0*q3);
  RotM->m[0][2] = 2.0F*(q1*q3+q0*q2);

  RotM->m[1][0] = 2.0F*(q1*q2+q0*q3);
  RotM->m[1][1] = q0*q0-q1*q1+q2*q2-q3*q3;
  RotM->m[1][2] = 2.0F*(q2*q3-q0*q1);

  RotM->m[2][0] = 2.0F*(q1*q3-q0*q2);
  RotM->m[2][1] = 2.0F*(q2*q3+q0*q1);
  RotM->m[2][2] = q0*q0-q1*q1-q2*q2+q3*q3;
}

typedef struct {
  union {
    float wordLreal;
    unsigned int wordLuint;
  } wordL;
} IEEESingle;

unsigned char IsNaNF(float value)
{
  IEEESingle tmp;
  tmp.wordL.wordLreal = value;
  return (unsigned char)( (tmp.wordL.wordLuint & 0x7F800000) == 0x7F800000 &&
                     (tmp.wordL.wordLuint & 0x007FFFFF) != 0 );
}

unsigned char IsInfF(float value)
{
  IEEESingle infF;
  infF.wordL.wordLuint = 0x7F800000U;
  IEEESingle minfF;
  minfF.wordL.wordLuint = 0xFF800000U;
  float rtInfF = infF.wordL.wordLreal;
  float rtMinusInfF = minfF.wordL.wordLuint;
  return (unsigned char)(((value)==rtInfF || (value)==rtMinusInfF) ? 1U : 0U);
}

float atan2f_snf(float u0, float u1)
{
  float y;
  int u0_0;
  int u1_0;
  if (IsNaNF(u0) || IsNaNF(u1)) {
    y = (float)0xFFC00000U;
  } else if (IsInfF(u0) && IsInfF(u1)) {
    if (u0 > 0.0F) {
      u0_0 = 1;
    } else {
      u0_0 = -1;
    }

    if (u1 > 0.0F) {
      u1_0 = 1;
    } else {
      u1_0 = -1;
    }

    y = atan2f((float)u0_0, (float)u1_0);
  } else if (u1 == 0.0F) {
    if (u0 > 0.0F) {
      y = 3.1415927F / 2.0F;
    } else if (u0 < 0.0F) {
      y = -(3.1415927F / 2.0F);
    } else {
      y = 0.0F;
    }
  } else {
    y = atan2f(u0, u1);
  }

  return y;
}

void controllerGimbal2DInit(void) {
  if (isInit) {
    return;
  }

  isInit = true;
}

#define GIMBAL2D_ATTITUDE_UPDATE_DT    (float)(1.0f/ATTITUDE_RATE)

void Gimbal2D_AlphaBetaEstimator()
{
  // rotation about z for 90 deg
  static const float qt[4] = { 0.707106769F, 0.0F, 0.0F, 0.707106769F };
  static const float qt_inv[4] = { -0.707106769F, 0.0F, 0.0F, 0.707106769F };
  float q_tmp[4], q_i[4], q_Bbi[4], q_Bbi_inv[4], q_bi[4], q_bi_inv[4], q_bii[4];
  Gimbal2D_quatmultiply((float*)&Gimbal2D_U.qw_IMU, qt_inv, q_tmp);
  Gimbal2D_quatmultiply(qt, q_tmp, q_i);
  
  // change of basis of base quat on ith qc
  float q_Bbi_tmp = -Gimbal2D_U.index * 3.14159274F / 4.0F;
  q_Bbi[0] = cosf(q_Bbi_tmp);
  q_Bbi[1] = 0.0F;
  q_Bbi[2] = 0.0F;
  q_Bbi[3] = sinf(q_Bbi_tmp);

  q_Bbi_inv[0] = -q_Bbi[0];
  q_Bbi_inv[1] = q_Bbi[1];
  q_Bbi_inv[2] = q_Bbi[2];
  q_Bbi_inv[3] = q_Bbi[3];

  // ith base quat
  Gimbal2D_quatmultiply((float*)&Gimbal2D_U.qw_Base, q_Bbi_inv, q_tmp);
  Gimbal2D_quatmultiply(q_Bbi, q_tmp, q_bi);
  
  // inv of ith base quat
  q_bi_inv[0] = -q_bi[0];
  q_bi_inv[1] = q_bi[1];
  q_bi_inv[2] = q_bi[2];
  q_bi_inv[3] = q_bi[3];

  // rotation from ith base to qc
  Gimbal2D_quatmultiply(q_bi_inv, q_i, q_bii);

  // change to R3x3
  struct mat33 RotM;
  Gimbal2D_quatToDCM(q_bii, &RotM);
  RotM = mtranspose(RotM);

  // incremental alpha-beta estimator
  float alpha0_e = atan2f_snf(RotM.m[1][2], RotM.m[1][1]);
  float beta0_e = atan2f_snf(RotM.m[2][0], RotM.m[0][0]);

  // Thrust Clamper
  if (Gimbal2D_U.thrust >
      Gimbal2D_P.ThrustUpperBound) {
    Gimbal2D_U.ClampedThrust = Gimbal2D_P.ThrustUpperBound;
  } else if (Gimbal2D_U.thrust <
             Gimbal2D_P.ThrustLowerBound) {
    Gimbal2D_U.ClampedThrust = Gimbal2D_P.ThrustLowerBound;
  } else {
    Gimbal2D_U.ClampedThrust = Gimbal2D_U.thrust;
  }

  if(Gimbal2D_U.ClampedThrust >= 0.000898f && Gimbal2D_U.LastThrust <= 0.000898f)
  {
    Gimbal2D_Y.acount_prev = 0.0F;
    Gimbal2D_Y.bcount_prev = 0.0F;
  }

  float diff_a = Gimbal2D_Y.alpha_prev - alpha0_e;
  float acount = Gimbal2D_Y.acount_prev;
  
  if (fabsf(diff_a) > 3.14159274F) {
    if (diff_a < 0.0F) {
      diff_a = -1.0F;
    } else if (diff_a > 0.0F) {
      diff_a = 1.0F;
    } else if (diff_a == 0.0F) {
      diff_a = 0.0F;
    }
    acount += diff_a;
  }
  Gimbal2D_Y.alpha_e = alpha0_e + 6.28318548F * acount;

  float diff_b = Gimbal2D_Y.beta_prev - beta0_e;
  float bcount = Gimbal2D_Y.bcount_prev;
  
  if (fabsf(diff_b) > 3.14159274F) {
    if (diff_b < 0.0F) {
      diff_b = -1.0F;
    } else if (diff_b > 0.0F) {
      diff_b = 1.0F;
    } else if (diff_b == 0.0F) {
      diff_b = 0.0F;
    }
    acount += diff_b;
  }
  Gimbal2D_Y.beta_e = beta0_e + 6.28318548F * bcount;

  // speed estimator
  Gimbal2D_Y.alpha_speed_e = Gimbal2D_U.omega_x * cosf(Gimbal2D_Y.beta_e) + Gimbal2D_U.omega_z * sinf(Gimbal2D_Y.beta_e);
  Gimbal2D_Y.beta_speed_e = Gimbal2D_U.beta_speed;

  // Last
  Gimbal2D_U.LastThrust = Gimbal2D_U.ClampedThrust;
  Gimbal2D_Y.acount_prev = acount;
  Gimbal2D_Y.bcount_prev = bcount;
  Gimbal2D_Y.alpha_prev = Gimbal2D_Y.alpha_e;
  Gimbal2D_Y.beta_prev = Gimbal2D_Y.beta_e; 
}

void Gimbal2D_controller()
{
  // Update your control law here
  // NSF Controller -- State Feedback
  float z1 = Gimbal2D_Y.alpha_e - Gimbal2D_U.alpha_desired;
  float z2 = Gimbal2D_Y.beta_e - Gimbal2D_U.beta_desired;
  float z3 = Gimbal2D_Y.alpha_speed_e;
  float z4 = Gimbal2D_Y.beta_speed_e;
  float num1 = ((Gimbal2D_P.J[0] + Gimbal2D_P.J[1] - Gimbal2D_P.J[2])*sinf(Gimbal2D_Y.beta_e) - Gimbal2D_P.J[2]*cosf(Gimbal2D_Y.beta_e))*z3*z4;
  float den1 = Gimbal2D_P.J[0]*cosf(Gimbal2D_Y.beta_e) + Gimbal2D_P.J[1]*sinf(Gimbal2D_Y.beta_e);
  float cir1 = num1 / den1;
  float num2 = (Gimbal2D_P.J[2] - Gimbal2D_P.J[0])*sinf(Gimbal2D_Y.beta_e)*cosf(Gimbal2D_Y.beta_e)*z3*z3;
  float den2 = Gimbal2D_P.J[1];
  float cir2 = num2 / den2;
  float star = 1.0f / den1;
  Gimbal2D_Y.utilt1 = - Gimbal2D_P.K[0][0]*z1 - Gimbal2D_P.K[0][2]*z3 - cir1;
  Gimbal2D_Y.utilt2 = - Gimbal2D_P.K[1][1]*z2 - Gimbal2D_P.K[1][3]*z4 - cir2;
  Gimbal2D_Y.u_u1 = Gimbal2D_Y.utilt1 / star; // Tau_x + Tau_z
  Gimbal2D_Y.u_u2 = Gimbal2D_Y.utilt2 * Gimbal2D_P.J[1]; // Tau_y
  Gimbal2D_Y.Tau_x = (cosf(Gimbal2D_Y.beta_e)*Gimbal2D_Y.u_u1 + tanf(Gimbal2D_Y.alpha_e)*Gimbal2D_Y.u_u2)/(sinf(Gimbal2D_Y.beta_e)+cosf(Gimbal2D_Y.beta_e));
  Gimbal2D_Y.Tau_y = Gimbal2D_Y.u_u2;
  Gimbal2D_Y.Tau_z = (sinf(Gimbal2D_Y.beta_e)*Gimbal2D_Y.u_u1 - tanf(Gimbal2D_Y.alpha_e)*Gimbal2D_Y.u_u2)/(sinf(Gimbal2D_Y.beta_e)+cosf(Gimbal2D_Y.beta_e));
}

void Gimbal2D_PowerDistribution()
{
    // power distribution and turn Nm into N
  const float arm = 0.707106781f * 0.046f;
  const float rollPart = 0.25f / arm * Gimbal2D_Y.Tau_x;
  const float pitchPart = 0.25f / arm * Gimbal2D_Y.Tau_y;
  const float thrustPart = 0.25f * Gimbal2D_U.ClampedThrust; // N (per rotor)
  const float yawPart = 0.25f * Gimbal2D_Y.Tau_z / 0.005964552f;

  Gimbal2D_Y.rollPart = rollPart;
  Gimbal2D_Y.pitchPart = pitchPart;
  Gimbal2D_Y.thrustPart = thrustPart;
  Gimbal2D_Y.yawPart = yawPart;

  // corresponding to i-frame Body coordinate, t_mi 's Unit is Newton
  Gimbal2D_Y.t_m1 = thrustPart + rollPart - pitchPart - yawPart;
  Gimbal2D_Y.t_m2 = thrustPart - rollPart - pitchPart + yawPart;
  Gimbal2D_Y.t_m3 = thrustPart - rollPart + pitchPart - yawPart;
  Gimbal2D_Y.t_m4 = thrustPart + rollPart + pitchPart + yawPart;

  Gimbal2D_Y.IsClamped = 0;
  if (Gimbal2D_Y.t_m1 < 0.0f) 
  {
    Gimbal2D_Y.t_m1 = 0.0f;
    Gimbal2D_Y.IsClamped = 1;
  } else if (Gimbal2D_Y.t_m1 > MOTOR_MAX_THRUST_N) {
    Gimbal2D_Y.t_m1 = MOTOR_MAX_THRUST_N;
    Gimbal2D_Y.IsClamped = 1;
  }

  if (Gimbal2D_Y.t_m2 < 0.0f) 
  {
    Gimbal2D_Y.t_m2 = 0.0f;
    Gimbal2D_Y.IsClamped = 1;
  } else if (Gimbal2D_Y.t_m2 > MOTOR_MAX_THRUST_N) {
    Gimbal2D_Y.t_m2 = MOTOR_MAX_THRUST_N;
    Gimbal2D_Y.IsClamped = 1;
  }

  if (Gimbal2D_Y.t_m3 < 0.0f) 
  {
    Gimbal2D_Y.t_m3 = 0.0f;
    Gimbal2D_Y.IsClamped = 1;
  } else if (Gimbal2D_Y.t_m3 > MOTOR_MAX_THRUST_N) {
    Gimbal2D_Y.t_m3 = MOTOR_MAX_THRUST_N;
    Gimbal2D_Y.IsClamped = 1;
  }

  if (Gimbal2D_Y.t_m4 < 0.0f) 
  {
    Gimbal2D_Y.t_m4 = 0.0f;
    Gimbal2D_Y.IsClamped = 1;
  } else if (Gimbal2D_Y.t_m4 > MOTOR_MAX_THRUST_N) {
    Gimbal2D_Y.t_m4 = MOTOR_MAX_THRUST_N;
    Gimbal2D_Y.IsClamped = 1;
  }

  // Turn Newton into percentage and count
  Gimbal2D_Y.m1 = Gimbal2D_Y.t_m1 / MOTOR_MAX_THRUST_N * 65535;
  Gimbal2D_Y.m2 = Gimbal2D_Y.t_m2 / MOTOR_MAX_THRUST_N * 65535;
  Gimbal2D_Y.m3 = Gimbal2D_Y.t_m3 / MOTOR_MAX_THRUST_N * 65535;
  Gimbal2D_Y.m4 = Gimbal2D_Y.t_m4 / MOTOR_MAX_THRUST_N * 65535;
}

void controllerGimbal2D(control_t *control,
                                 const setpoint_t *setpoint,
                                 const sensorData_t *sensors,
                                 const state_t *state,
                                 const stabilizerStep_t stabilizerStep) 
{
  // Update Command (Send2D)
  Gimbal2D_U.index = setpoint->attitude.roll;

  Gimbal2D_U.qw_Base = setpoint->attitudeQuaternion.w;
  Gimbal2D_U.qx_Base = setpoint->attitudeQuaternion.x;
  Gimbal2D_U.qy_Base = setpoint->attitudeQuaternion.y;
  Gimbal2D_U.qz_Base = setpoint->attitudeQuaternion.z;

  Gimbal2D_U.alpha_desired = setpoint->attitude.pitch;
  Gimbal2D_U.beta_desired = setpoint->attitude.yaw;
  Gimbal2D_U.thrust = setpoint->thrust;

  // Update Feedback
  Gimbal2D_U.qw_IMU = state->attitudeQuaternion.w;
  Gimbal2D_U.qx_IMU = state->attitudeQuaternion.x;
  Gimbal2D_U.qy_IMU = state->attitudeQuaternion.y;
  Gimbal2D_U.qz_IMU = state->attitudeQuaternion.z;

  Gimbal2D_U.omega_x = -radians(sensors->gyro.y);
  Gimbal2D_U.beta_speed = radians(sensors->gyro.x);
  Gimbal2D_U.omega_z = radians(sensors->gyro.z);

  Gimbal2D_AlphaBetaEstimator();
  Gimbal2D_controller();
  Gimbal2D_PowerDistribution();

  // Update the output
  if (setpoint->thrust < 0.000898f)
  {
    motorsSetRatio(0, 0);
    motorsSetRatio(1, 0);
    motorsSetRatio(2, 0);
    motorsSetRatio(3, 0);    
  }
  else
  {
    motorsSetRatio(0, Gimbal2D_Y.m1);
    motorsSetRatio(1, Gimbal2D_Y.m2);
    motorsSetRatio(2, Gimbal2D_Y.m3);
    motorsSetRatio(3, Gimbal2D_Y.m4);
  }
}

bool controllerGimbal2DTest(void) {
  return true;
}

// Update your parameter here
PARAM_GROUP_START(sparam_Gimbal2D)
PARAM_ADD(PARAM_FLOAT, Kp, &Gimbal2D_P.Kp)
// Parameters for K and B_inv and J (Needs debugging. Why cannot set by Python code?)
PARAM_ADD(PARAM_FLOAT, K11, &Gimbal2D_P.K[0][0])
PARAM_ADD(PARAM_FLOAT, K12, &Gimbal2D_P.K[0][1])
PARAM_ADD(PARAM_FLOAT, K13, &Gimbal2D_P.K[0][2])
PARAM_ADD(PARAM_FLOAT, K14, &Gimbal2D_P.K[0][3])
PARAM_ADD(PARAM_FLOAT, K21, &Gimbal2D_P.K[1][0])
PARAM_ADD(PARAM_FLOAT, K22, &Gimbal2D_P.K[1][1])
PARAM_ADD(PARAM_FLOAT, K23, &Gimbal2D_P.K[1][2])
PARAM_ADD(PARAM_FLOAT, K24, &Gimbal2D_P.K[1][3])

PARAM_ADD(PARAM_FLOAT, B_inv11, &Gimbal2D_P.B_inv[0][0])
PARAM_ADD(PARAM_FLOAT, B_inv12, &Gimbal2D_P.B_inv[0][1])
PARAM_ADD(PARAM_FLOAT, B_inv13, &Gimbal2D_P.B_inv[0][2])
PARAM_ADD(PARAM_FLOAT, B_inv14, &Gimbal2D_P.B_inv[0][3])
PARAM_ADD(PARAM_FLOAT, B_inv21, &Gimbal2D_P.B_inv[1][0])
PARAM_ADD(PARAM_FLOAT, B_inv22, &Gimbal2D_P.B_inv[1][1])
PARAM_ADD(PARAM_FLOAT, B_inv23, &Gimbal2D_P.B_inv[1][2])
PARAM_ADD(PARAM_FLOAT, B_inv24, &Gimbal2D_P.B_inv[1][3])

PARAM_ADD(PARAM_FLOAT, Jx, &Gimbal2D_P.J[0])
PARAM_ADD(PARAM_FLOAT, Jy, &Gimbal2D_P.J[1])
PARAM_ADD(PARAM_FLOAT, Jz, &Gimbal2D_P.J[2])
////////
PARAM_GROUP_STOP(sparam_Gimbal2D)

/**
 * Update your debug scope here
 * Logging variables for the command and reference signals for the
 * Gimbal2D controller
 */
LOG_GROUP_START(sctrl_Gimbal2D)
LOG_ADD(LOG_FLOAT, alpha, &Gimbal2D_Y.alpha_e)
LOG_ADD(LOG_FLOAT, alphas, &Gimbal2D_Y.alpha_speed_e)
LOG_ADD(LOG_FLOAT, beta, &Gimbal2D_Y.beta_e)
LOG_ADD(LOG_FLOAT, betas, &Gimbal2D_Y.beta_speed_e)

LOG_ADD(LOG_FLOAT, t_m1, &Gimbal2D_Y.t_m1)
LOG_ADD(LOG_FLOAT, t_m2, &Gimbal2D_Y.t_m2)
LOG_ADD(LOG_FLOAT, t_m3, &Gimbal2D_Y.t_m3)
LOG_ADD(LOG_FLOAT, t_m4, &Gimbal2D_Y.t_m4)
//// NSF
LOG_ADD(LOG_FLOAT, u1, &Gimbal2D_Y.u_u1)
LOG_ADD(LOG_FLOAT, u2, &Gimbal2D_Y.u_u2)
LOG_ADD(LOG_FLOAT, utilt1, &Gimbal2D_Y.utilt1)
LOG_ADD(LOG_FLOAT, utilt2, &Gimbal2D_Y.utilt2)
////
LOG_GROUP_STOP(sctrl_Gimbal2D)