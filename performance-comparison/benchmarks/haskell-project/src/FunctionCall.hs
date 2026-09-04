-- function_call -- N calls through a NOINLINE function, measuring call
-- overhead rather than a fused loop.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
inc1 :: Int -> Int
inc1 x = x + 1
{-# NOINLINE inc1 #-}
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000000 :: Int
      go !i !v | i >= n = v
               | otherwise = go (i + 1) (inc1 v)
  print (go 0 0)
