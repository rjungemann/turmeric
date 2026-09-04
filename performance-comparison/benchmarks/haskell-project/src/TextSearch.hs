-- text_search -- x/hello haystack, non-overlapping "hello" count over a
-- strict ByteString (byte-oriented like the Turmeric column).
{-# LANGUAGE BangPatterns, OverloadedStrings #-}
import System.Environment (getArgs)
import qualified Data.ByteString as BS
import qualified Data.ByteString.Char8 as BC
main :: IO ()
main = do
  args <- getArgs
  let hsSize = case args of (a:_) -> read a; _ -> 10000 :: Int
      needle = "hello" :: BS.ByteString
      hay = BC.pack [ if i `mod` 10 < 5 then 'x' else "hello" !! (i `mod` 10 - 5)
                    | i <- [0 .. hsSize - 1] ]
      count !acc h =
        let (_, rest) = BS.breakSubstring needle h
        in if BS.null rest then acc
           else count (acc + 1) (BS.drop (BS.length needle) rest)
  print (count (0 :: Int) hay)
