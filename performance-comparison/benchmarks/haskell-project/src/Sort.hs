-- sort -- LCG fill (logical >>1), sort ascending, print min and max.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Data.Word (Word64)
import Data.List (sort, foldl')
lcgA, lcgC :: Word64
lcgA = 6364136223846793005
lcgC = 1442695040888963407
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000 :: Int
      fill !i !st acc | i >= n = acc
                      | otherwise =
                          let s = st * lcgA + lcgC
                          in fill (i + 1) s ((s `div` 2) : acc)
      arr = sort (fill 0 12345 [])
  putStrLn (show (head arr) ++ " " ++ show (last arr))
