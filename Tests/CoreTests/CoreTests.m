// XCTest wrapper: runs the C core test suite inside `xcodebuild test`.
#import <XCTest/XCTest.h>

extern int dsxp_core_tests_run(void);

@interface CoreTests : XCTestCase
@end

@implementation CoreTests

- (void)testCoreSuite {
    XCTAssertEqual(dsxp_core_tests_run(), 0, @"see the test log for FAIL lines");
}

@end
