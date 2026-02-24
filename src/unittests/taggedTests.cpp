#include "gtest/gtest.h"
#include "poolcore/backendData.h"
#include "poolcore/poolCore.h"

struct PresenceProbeRecord : CSerializable<PresenceProbeRecord> {
  uint32_t A = 0;
  uint32_t B = 0;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &PresenceProbeRecord::A>(),
      field<2, &PresenceProbeRecord::B>()
    );
  }
};

struct OptionalProbeRecord : CSerializable<OptionalProbeRecord> {
  std::optional<uint32_t> A;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &OptionalProbeRecord::A>()
    );
  }
};

struct ScalarProbeRecord : CSerializable<ScalarProbeRecord> {
  uint32_t A = 0;

  static constexpr auto schema() {
    return std::make_tuple(
      field<1, &ScalarProbeRecord::A>()
    );
  }
};

TEST(Tagged, UserSettingsRecordRoundTrip)
{
  // Create a record like migration does
  UserSettingsRecord original;
  original.Login = "testuser";
  original.Coin = "BTC.regtest";
  original.Payout.Mode = EPayoutMode::Disabled;
  original.Payout.Address = "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa";
  original.Payout.InstantPayoutThreshold = UInt<384>(1000000u);
  original.Mining.MiningMode = EMiningMode::PPLNS;
  original.AutoExchange.PayoutCoinName = "";

  // Serialize
  xmstream stream;
  original.serializeValue(stream);

  // Deserialize
  UserSettingsRecord loaded;
  bool ok = loaded.deserializeValue(stream.data(), stream.sizeOf());

  ASSERT_TRUE(ok) << "deserializeValue failed for valid tagged data";
  EXPECT_EQ(loaded.Login, "testuser");
  EXPECT_EQ(loaded.Coin, "BTC.regtest");
  EXPECT_EQ(loaded.Payout.Mode, EPayoutMode::Disabled);
  EXPECT_EQ(loaded.Payout.Address, "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa");
  EXPECT_EQ(loaded.Payout.InstantPayoutThreshold, UInt<384>(1000000u));
  EXPECT_EQ(loaded.Mining.MiningMode, EMiningMode::PPLNS);
  EXPECT_EQ(loaded.AutoExchange.PayoutCoinName, "");
}

TEST(Tagged, UserSettingsRecordInstantMode)
{
  UserSettingsRecord original;
  original.Login = "miner42";
  original.Coin = "XPM.regtest";
  original.Payout.Mode = EPayoutMode::Instant;
  original.Payout.Address = "ATWDYBwVDvswyZADMbEo5yBt4tH2zfGjd1";
  original.Payout.InstantPayoutThreshold = UInt<384>(500u);
  original.Mining.MiningMode = EMiningMode::PPS;
  original.AutoExchange.PayoutCoinName = "BTC";

  xmstream stream;
  original.serializeValue(stream);

  UserSettingsRecord loaded;
  bool ok = loaded.deserializeValue(stream.data(), stream.sizeOf());

  ASSERT_TRUE(ok);
  EXPECT_EQ(loaded.Payout.Mode, EPayoutMode::Instant);
  EXPECT_EQ(loaded.Payout.Address, "ATWDYBwVDvswyZADMbEo5yBt4tH2zfGjd1");
  EXPECT_EQ(loaded.Mining.MiningMode, EMiningMode::PPS);
  EXPECT_EQ(loaded.AutoExchange.PayoutCoinName, "BTC");
}

TEST(Tagged, UserSettingsRecordEmptyFields)
{
  UserSettingsRecord original;
  original.Login = "u";
  original.Coin = "A";
  // All other fields at defaults

  xmstream stream;
  original.serializeValue(stream);

  UserSettingsRecord loaded;
  bool ok = loaded.deserializeValue(stream.data(), stream.sizeOf());

  ASSERT_TRUE(ok);
  EXPECT_EQ(loaded.Login, "u");
  EXPECT_EQ(loaded.Coin, "A");
  EXPECT_EQ(loaded.Payout.Mode, EPayoutMode::Disabled);
  EXPECT_EQ(loaded.Payout.Address, "");
}

TEST(Tagged, PresenceAndDefaultsResetOnReuse)
{
  xmstream full;
  writeTagged(full, 1u, static_cast<uint32_t>(10));
  writeTagged(full, 2u, static_cast<uint32_t>(20));

  xmstream partial;
  writeTagged(partial, 1u, static_cast<uint32_t>(33));

  PresenceProbeRecord loaded;

  xmstream fullIn(full.data(), full.sizeOf());
  ASSERT_TRUE(unserialize(fullIn, loaded));
  ASSERT_TRUE(loaded.has<&PresenceProbeRecord::A>());
  ASSERT_TRUE(loaded.has<&PresenceProbeRecord::B>());
  ASSERT_EQ(loaded.A, 10u);
  ASSERT_EQ(loaded.B, 20u);

  xmstream partialIn(partial.data(), partial.sizeOf());
  ASSERT_TRUE(unserialize(partialIn, loaded));
  EXPECT_TRUE(loaded.has<&PresenceProbeRecord::A>());
  EXPECT_FALSE(loaded.has<&PresenceProbeRecord::B>());
  EXPECT_EQ(loaded.A, 33u);
  EXPECT_EQ(loaded.B, 0u);
}

TEST(Tagged, InvalidOptionalPayloadFails)
{
  xmstream stream;
  stream.write(static_cast<uint32_t>(1));   // tag
  stream.write(static_cast<uint64_t>(1));   // payload size
  stream.write(static_cast<uint8_t>(0xAA)); // truncated uint32 payload

  OptionalProbeRecord loaded;
  xmstream input(stream.data(), stream.sizeOf());
  EXPECT_FALSE(unserialize(input, loaded));
}

TEST(Tagged, OversizedFieldFails)
{
  xmstream stream;
  stream.write(static_cast<uint32_t>(1));    // tag
  stream.write(static_cast<uint64_t>(8));    // claims 8-byte payload
  stream.write(static_cast<uint32_t>(123));  // actual payload: 4 bytes

  ScalarProbeRecord loaded;
  xmstream input(stream.data(), stream.sizeOf());
  EXPECT_FALSE(unserialize(input, loaded));
}

TEST(Tagged, TrailingBytesFail)
{
  ScalarProbeRecord original;
  original.A = 7;

  xmstream stream;
  serialize(stream, original);
  stream.write(static_cast<uint8_t>(0xFF)); // trailing garbage

  ScalarProbeRecord loaded;
  xmstream input(stream.data(), stream.sizeOf());
  EXPECT_FALSE(unserialize(input, loaded));
}
