-- factorial -- tail-recursive accumulator mod 1e9+7 at every step.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
modulus :: Int
modulus = 1000000007
fact :: Int -> Int -> Int
fact !n !acc | n <= 1 = acc `mod` modulus
             | otherwise = fact (n - 1) ((acc * n) `mod` modulus)
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000 :: Int
  print (fact n 1)
