// SPDX-License-Identifier: AGPL-3.0-or-later
// Filter laws fitted to our own recordings of the reference's audio output (measurement run va-round5, batches F1-F3: a held saw
// through each filter against the same saw through thru; F2: key follow and velocity), written as knot tables by our analysis;
// laws.cpp says how they are used.
// The cutoff law: fc (Hz) every 8 raw steps (F3: LPF12, the section's F and q fitted within 0.001 dB); log-frequency between.
constexpr float kFcX[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 128, 136, 144, 152, 160, 168, 176, 184, 192, 200, 208, 216, 224, 232, 240, 248, 256, 264, 272, 280, 288, 296, 304, 312, 320, 328, 336, 344, 352, 360, 368, 376, 384, 392, 400, 408, 416, 424, 432, 440, 448, 456, 464, 472, 480, 488, 496, 504, 512, 520, 528, 536, 544, 552, 560, 568, 576, 584, 592, 600, 608, 616, 624, 632, 640, 648, 656, 664, 672, 680, 688, 696, 704, 712, 720, 728, 736, 744, 752, 760, 768, 776, 784, 792, 800, 808, 816, 824, 832, 840, 848, 856, 864, 872, 880, 888, 896, 904, 912, 920, 928, 936, 944, 952, 960, 968, 976, 984, 992, 1000, 1008, 1016, 1023};
constexpr float kFcY[] = {2.57f, 2.57f, 3.853f, 3.853f, 3.853f, 3.855f, 5.141f, 5.141f, 5.139f, 5.14f, 5.138f, 6.425f, 6.425f, 7.71f, 8.994f, 8.998f, 10.711f, 10.712f, 10.705f, 11.994f, 11.995f, 13.284f, 14.561f, 15.85f, 17.144f, 18.423f, 20.132f, 21.433f, 22.73f, 24.01f, 24.02f, 25.3f, 26.59f, 30.88f, 33.46f, 36.04f, 38.61f, 41.6f, 44.19f, 46.76f, 48.07f, 51.08f, 52.37f, 56.23f, 63.11f, 69.58f, 75.19f, 80.33f, 85.96f, 91.13f, 95.42f, 99.3f, 103.62f, 107.51f, 119.62f, 133.02f, 145.12f, 155.95f, 166.79f, 175.91f, 185.44f, 194.99f, 202.83f, 211.09f, 219.35f, 247.26f, 274.3f, 298.37f, 320.3f, 341.82f, 360.66f, 379.15f, 396.78f, 413.11f, 427.69f, 460.0f, 515.06f, 565.0f, 613.33f, 655.1f, 697.0f, 734.52f, 770.78f, 804.42f, 838.14f, 869.22f, 959.22f, 1065.67f, 1165.11f, 1255.45f, 1341.73f, 1420.94f, 1496.41f, 1568.97f, 1633.66f, 1699.77f, 1765.71f, 1992.64f, 2208.08f, 2403.27f, 2583.5f, 2751.87f, 2912.07f, 3058.72f, 3208.5f, 3340.0f, 3467.93f, 3730.64f, 4186.86f, 4605.3f, 5012.91f, 5380.98f, 5735.81f, 6068.4f, 6386.0f, 6693.6f, 6986.4f, 7269.2f, 8119.7f, 9163.0f, 10195.7f, 11179.0f, 12202.6f, 13217.6f, 14300.2f, 15481.5f, 16759.7f, 18391.6f, 20565.7f};
constexpr int kFcN = sizeof kFcX / sizeof kFcX[0];
// The resonance's pass-band gain (dB; F3: the same within 0.01 dB at cutoffs 340-1000 on LPF12, F1: LPF24 and LPF18 alike).
constexpr float kResGainX[] = {0, 8, 16, 24, 32, 48, 64, 96, 128, 160, 192, 224, 255};
constexpr float kResGainY[] = {0.000f, -0.469f, -0.934f, -1.432f, -1.977f, -3.067f, -3.741f, -4.800f, -5.810f, -6.870f, -8.063f, -10.232f, -13.578f};
constexpr int kResGainN = sizeof kResGainX / sizeof kResGainX[0];
// The high-passes' output gains (dB) by log2 fc x 12 (HPF24, HPF18, HPF12, HPF6; F1, F3: the shape within 0.005 dB, the gain
// fitted on each probe), held at the ends.
constexpr float kHp24X[] = {16.542f, 33.688f, 50.420f, 58.116f, 65.075f, 68.204f, 69.155f, 70.503f, 73.452f, 76.521f, 78.911f, 80.653f, 84.663f, 90.414f, 95.401f, 99.263f, 106.146f, 115.451f, 124.678f, 132.414f, 140.468f, 150.317f, 159.788f};
constexpr float kHp24Y[] = {-0.650f, -1.218f, -1.979f, -2.422f, -2.919f, -3.146f, -3.252f, -3.345f, -3.579f, -3.852f, -4.059f, -4.128f, -4.606f, -5.228f, -5.801f, -6.277f, -7.205f, -8.637f, -10.303f, -11.934f, -13.919f, -16.879f, -20.521f};
constexpr float kHp18X[] = {69.155f, 80.653f, 90.414f, 99.263f, 106.146f, 115.451f, 124.678f, 132.414f, 140.468f, 150.317f, 159.788f};
constexpr float kHp18Y[] = {-2.474f, -3.392f, -4.149f, -4.758f, -5.310f, -6.036f, -6.651f, -6.975f, -6.944f, -5.987f, -3.330f};
constexpr float kHp12X[] = {16.542f, 23.562f, 28.206f, 32.137f, 41.034f, 43.020f, 49.151f, 54.055f, 56.800f, 63.246f, 67.056f, 71.755f, 77.115f, 80.339f, 86.171f, 90.414f, 93.329f, 99.879f, 103.587f, 108.105f, 113.340f, 116.532f, 122.235f, 126.567f, 129.432f, 136.021f, 139.772f, 144.380f, 149.829f, 153.244f, 159.788f, 165.645f};
constexpr float kHp12Y[] = {-0.224f, -0.321f, -0.422f, -0.707f, -0.704f, -0.766f, -0.880f, -1.211f, -1.078f, -1.405f, -1.424f, -1.652f, -1.959f, -2.028f, -2.358f, -2.581f, -2.713f, -3.120f, -3.530f, -3.862f, -4.281f, -4.554f, -5.085f, -5.526f, -5.838f, -6.629f, -7.130f, -7.806f, -8.709f, -9.345f, -10.752f, -12.299f};
constexpr float kHp6X[] = {69.155f, 80.653f, 90.414f, 99.263f, 106.146f, 115.451f, 124.678f, 132.414f, 140.468f, 150.317f, 159.788f};
constexpr float kHp6Y[] = {-0.806f, -1.258f, -1.264f, -1.526f, -1.598f, -1.580f, -1.326f, -0.794f, 0.279f, 2.807f, 7.419f};
constexpr const float* kHpTabX[4] = {kHp24X, kHp18X, kHp12X, kHp6X};
constexpr const float* kHpTabY[4] = {kHp24Y, kHp18Y, kHp12Y, kHp6Y};
constexpr int kHpTabN[4] = {int(sizeof kHp24X / sizeof(float)), int(sizeof kHp18X / sizeof(float)), int(sizeof kHp12X / sizeof(float)), int(sizeof kHp6X / sizeof(float))};
// Key follow: semitones a key from key 60 (F2: 1/3, 1/2, 2/3, 1 and 2 octaves an octave, keys 36-96, within 0.03 semitone).
constexpr float kKeyFollowSemis[6] = {0, 1.f / 3, 0.5f, 2.f / 3, 1, 2};
// Velocity (F2, key 60): the cutoff moves by (sensitivity - 256) x kVelCut(v) semitones (sensitivities -255..+255 at velocities
// 1/32/64/96/127, mirrored for a negative sensitivity, within 1.1 semitone; under 20 Hz not resolved), the EG depth is
// multiplied by 1 + (sensitivity - 256) x kVelFeg(v) (+-128 and +255 at velocities 1/64/127, within 0.012). A negative
// sensitivity reads the velocity mirrored (128 - v).
constexpr float kVelX[] = {1, 32, 64, 96, 127};
constexpr float kVelCut[] = {-0.559f, -0.420f, -0.256f, -0.096f, 0.065f};
constexpr float kVelFegX[] = {1, 64, 127}, kVelFeg[] = {-0.00391f, -0.001775f, 0.0004f};
