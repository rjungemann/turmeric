-- fibonacci (iterative) -- strict accumulators; Int is 64-bit and wraps,
-- matching the C/Turmeric columns (no Integer bignum).
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000 :: Int
  print (fib n)
fib :: Int -> Int
fib n | n <= 1 = n
      | otherwise = go 2 0 1
  where
    go :: Int -> Int -> Int -> Int
    go !i !a !b | i > n = b
                | otherwise = go (i + 1) b (a + b)
