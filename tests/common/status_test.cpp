#include "common/status.h"
#include <gtest/gtest.h>

using namespace anycache;

TEST(StatusTest, OkStatus) {
  Status s = Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, ErrorStatus) {
  Status s = Status::NotFound("file not found");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_EQ(s.code(), StatusCode::kNotFound);
  EXPECT_EQ(s.message(), "file not found");
  EXPECT_EQ(s.ToString(), "NotFound: file not found");
}

TEST(StatusTest, IOError) {
  Status s = Status::IOError("disk failure");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsIOError());
}

TEST(StatusTest, AlreadyExists) {
  Status s = Status::AlreadyExists("path exists");
  EXPECT_TRUE(s.IsAlreadyExists());
}

TEST(StatusTest, ReturnIfError) {
  auto fn = []() -> Status {
    RETURN_IF_ERROR(Status::OK());
    RETURN_IF_ERROR(Status::NotFound("oops"));
    return Status::OK(); // Should not reach here
  };
  Status s = fn();
  EXPECT_TRUE(s.IsNotFound());
}
