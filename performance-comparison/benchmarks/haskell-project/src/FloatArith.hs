-- float_arith -- FP ladder, both updates from the OLD a and b.  Printed
-- with an EXACT %.6f: GHC's printf/showFFloat format via the double's
-- shortest decimal representation (...7355 -> "735500"), where C expands
-- the exact binary value ("735474"); Rational arithmetic reproduces C.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
formatF6 :: Double -> String
formatF6 x =
  let scaled = round (toRational x * 1000000) :: Integer
      (q, r) = scaled `quotRem` 1000000
      frac = show (abs r)
      pad = replicate (6 - length frac) '0' ++ frac
      sign = if scaled < 0 && q == 0 then "-" else ""
  in sign ++ show q ++ "." ++ pad
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000000 :: Int
      go :: Int -> Double -> Double -> Double
      go !i !a !b
        | i >= n = a + b
        | otherwise = go (i + 1) (a * 1.0000001 + sqrt b) (b * 0.9999999 + sqrt a)
  putStrLn (formatF6 (go 0 1.0 1.0))
