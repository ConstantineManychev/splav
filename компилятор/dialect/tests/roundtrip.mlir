// CHECK-LABEL: func.func @sum(
// CHECK-SAME: %[[LEFT:.*]]: i32, %[[RIGHT:.*]]: i32) -> i32
// CHECK-NEXT: %[[SUM:.*]] = splav.add %[[LEFT]], %[[RIGHT]] : i32
// CHECK-NEXT: return %[[SUM]] : i32
func.func @sum(%left: i32, %right: i32) -> i32 {
  %sum = splav.add %left, %right : i32
  return %sum : i32
}
