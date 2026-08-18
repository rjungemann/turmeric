-- string_concat -- append "hello" N times, print the final length.
-- Strict ByteString via Builder (lazy String would measure list cells,
-- not string appends -- docs/methodology.md).
{-# LANGUAGE BangPatterns, OverloadedStrings #-}
import System.Environment (getArgs)
import qualified Data.ByteString.Builder as B
import qualified Data.ByteString.Lazy as BL
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1000 :: Int
      go !i acc | i >= n = acc
                | otherwise = go (i + 1) (acc <> B.byteString "hello")
  print (BL.length (B.toLazyByteString (go 0 mempty)))
