-- monte_carlo_pi -- same LCG, doubles from the top 53 bits, %.6f.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Data.Word (Word64)
import Text.Printf (printf)
lcgA, lcgC :: Word64
lcgA = 6364136223846793005
lcgC = 1442695040888963407
main :: IO ()
main = do
  args <- getArgs
  let iters = case args of (a:_) -> read a; _ -> 1000 :: Int
      denom = fromIntegral (2 ^ (53 :: Int) :: Word64) :: Double
      go :: Int -> Word64 -> Int -> Int
      go !i !st !inside
        | i >= iters = inside
        | otherwise =
            let s1 = st * lcgA + lcgC
                x  = fromIntegral (s1 `div` 2048) / denom
                s2 = s1 * lcgA + lcgC
                y  = fromIntegral (s2 `div` 2048) / denom
            in go (i + 1) s2 (if x * x + y * y <= 1.0 then inside + 1 else inside)
      inside = go 0 lcgA 0
  printf "%.6f\n" (4.0 * fromIntegral inside / fromIntegral iters :: Double)
