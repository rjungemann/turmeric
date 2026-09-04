-- list_ops -- prepend N ints to a linked list, sum by walking it.  Haskell
-- lists ARE cons cells, so the natural list is also the honest one; the
-- sum is a strict left fold (foldl').
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Data.List (foldl')
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000 :: Int
      lst = build 0 []
      build !i acc | i >= n = acc
                   | otherwise = build (i + 1) (i : acc)
  print (foldl' (+) 0 lst)
