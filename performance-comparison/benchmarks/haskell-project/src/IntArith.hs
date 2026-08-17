-- int_arith -- wrapping Word64 multiply-add ladder; b reads the updated a
-- (sequential).  Printed as signed 64-bit like C's %lld.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Data.Word (Word64)
import Data.Int (Int64)
import Data.Bits (xor)
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000000 :: Int
      go :: Int -> Word64 -> Word64 -> (Word64, Word64)
      go !i !a !b
        | i >= n = (a, b)
        | otherwise =
            let a' = a * 1000003 + b
                b' = b * 999983 + a'
            in go (i + 1) a' b'
      (fa, fb) = go 0 1 1
  print (fromIntegral (fa `xor` fb) :: Int64)
