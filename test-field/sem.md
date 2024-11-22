```c
    {
        _lock_sem(&log.sem);
        if (!_get_sem(&log.sem)) {
            _post_sem(&log.sem);
        }
        _unlock_sem(&log.sem);
    }
```
(trace) throughput = 79933.50 txn/s
(trace) throughput = 65773.00 txn/s
(trace) throughput = 93759.50 txn/s
(trace) throughput = 99021.00 txn/s
(trace) throughput = 95696.50 txn/s
(trace) throughput = 95552.50 txn/s
(trace) throughput = 81467.50 txn/s
(trace) throughput = 95416.00 txn/s
(trace) throughput = 105350.00 txn/s
(trace) throughput = 95824.00 txn/s
(trace) throughput = 93284.50 txn/s
(trace) throughput = 86718.00 txn/s
(trace) throughput = 95242.50 txn/s
(trace) throughput = 83394.50 txn/s
(trace) throughput = 84696.00 txn/s
(trace) throughput = 105115.00 txn/s
(trace) throughput = 88516.50 txn/s
(trace) throughput = 86710.00 txn/s
(trace) throughput = 83105.00 txn/s
(trace) throughput = 85461.50 txn/s
(trace) throughput = 77372.50 txn/s
(trace) throughput = 81097.00 txn/s
(trace) throughput = 74190.00 txn/s
(trace) throughput = 75943.50 txn/s
(trace) throughput = 88567.00 txn/s
(trace) throughput = 86467.00 txn/s
(trace) throughput = 93147.50 txn/s
(trace) throughput = 83136.50 txn/s
(trace) throughput = 113325.00 txn/s
(trace) throughput = 81507.00 txn/s
(trace) running: 30/30 (0 replayed)
(info) "banker" passed.
(info) OK: 23 tests passed.
第一次测试的平均数: 88493.00, 标准差: 9924.80

```c
    {
        _lock_sem(&log.sem);
        if (_query_sem(&log.sem) <= 0) {
            _post_sem(&log.sem);
        }
        _unlock_sem(&log.sem);
    }
```
(trace) throughput = 82656.00 txn/s
(trace) throughput = 75901.50 txn/s
(trace) throughput = 85825.00 txn/s
(trace) throughput = 65245.50 txn/s
(trace) throughput = 68883.50 txn/s
(trace) throughput = 69639.00 txn/s
(trace) throughput = 89382.50 txn/s
(trace) throughput = 76076.50 txn/s
(trace) throughput = 73739.50 txn/s
(trace) throughput = 75570.50 txn/s
(trace) throughput = 86839.00 txn/s
(trace) throughput = 82574.50 txn/s
(trace) throughput = 87773.00 txn/s
(trace) throughput = 102096.00 txn/s
(trace) throughput = 86466.00 txn/s
(trace) throughput = 86729.50 txn/s
(trace) throughput = 69165.50 txn/s
(trace) throughput = 90415.50 txn/s
(trace) throughput = 80078.50 txn/s
(trace) throughput = 77807.00 txn/s
(trace) throughput = 98923.00 txn/s
(trace) throughput = 73266.73 txn/s
(trace) throughput = 75734.50 txn/s
(trace) throughput = 93469.50 txn/s
(trace) throughput = 100081.00 txn/s
(trace) throughput = 82854.50 txn/s
(trace) throughput = 103935.00 txn/s
(trace) throughput = 105665.00 txn/s
(trace) throughput = 109677.50 txn/s
(trace) throughput = 91000.50 txn/s
(trace) running: 30/30 (0 replayed)
(info) "banker" passed.
(info) OK: 23 tests passed.
第二次测试的平均数: 84915.71, 标准差: 11706.59


- Once more with no `memset()` in `cache_acquire()`

(trace) throughput = 91172.00 txn/s
(trace) throughput = 71104.00 txn/s
(trace) throughput = 87027.50 txn/s
(trace) throughput = 96605.50 txn/s
(trace) throughput = 93006.50 txn/s
(trace) throughput = 74532.97 txn/s
(trace) throughput = 86747.00 txn/s
(trace) throughput = 89604.50 txn/s
(trace) throughput = 88905.50 txn/s
(trace) throughput = 74741.00 txn/s
(trace) throughput = 96948.00 txn/s
(trace) throughput = 95408.00 txn/s
(trace) throughput = 98735.50 txn/s
(trace) throughput = 88005.00 txn/s
(trace) throughput = 112287.50 txn/s
(trace) throughput = 109885.50 txn/s
(trace) throughput = 118689.16 txn/s
(trace) throughput = 92870.50 txn/s
(trace) throughput = 102815.50 txn/s
(trace) throughput = 82178.50 txn/s
(trace) throughput = 82866.00 txn/s
(trace) throughput = 85258.50 txn/s
(trace) throughput = 94521.50 txn/s
(trace) throughput = 105414.50 txn/s
(trace) throughput = 92453.50 txn/s
(trace) throughput = 98686.50 txn/s
(trace) throughput = 111198.20 txn/s
(trace) throughput = 85289.00 txn/s
(trace) throughput = 91861.50 txn/s
(trace) throughput = 93879.00 txn/s
